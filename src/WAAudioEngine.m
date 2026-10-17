#import "WAAudioEngine.h"
#import "WACommon.h"
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include "opus.h"
#include "wa_mlow.h"
#include "wa_mlow_decoder.h"
#include <pthread.h>

#define WA_SR 16000
#define WA_FRAME 320        // 20 ms at 16 kHz
#define WA_RING_SAMPLES (WA_FRAME * 32) // 640 ms sample ring buffer

@implementation WAAudioEngine {
    AudioUnit _unit;
    OpusEncoder *_enc;
    OpusDecoder *_dec;
    wa_mlow_decoder _mlow;   // MLow (WhatsApp default) inbound decoder, stateful across frames
    // playback sample ring buffer (handles arbitrary hardware IO buffer sizes like 256/512)
    int16_t _ring[WA_RING_SAMPLES];
    int _ringHead, _ringTail, _ringCount;
    pthread_mutex_t _ringLock;
    // capture accumulation buffer (accumulates exactly 320 samples for 20 ms Opus frames)
    int16_t _capBuf[WA_FRAME];
    int _capFill;
    BOOL _running;
}

- (instancetype)init
{
    if ((self = [super init])) {
        pthread_mutex_init(&_ringLock, NULL);
        wa_mlow_decoder_init(&_mlow);
        int err = 0;
        _enc = opus_encoder_create(WA_SR, 1, OPUS_APPLICATION_VOIP, &err);
        _dec = opus_decoder_create(WA_SR, 1, &err);
        if (_enc) {
            opus_encoder_ctl(_enc, OPUS_SET_BITRATE(24000));
            opus_encoder_ctl(_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
            opus_encoder_ctl(_enc, OPUS_SET_VBR(1));
        }
    }
    return self;
}

- (void)dealloc
{
    [self stop];
    if (_enc) opus_encoder_destroy(_enc);
    if (_dec) opus_decoder_destroy(_dec);
    pthread_mutex_destroy(&_ringLock);
}

// --- playback ring ---

- (void)pushSamples:(const int16_t *)pcm count:(int)n
{
    if (n <= 0) return;
    pthread_mutex_lock(&_ringLock);
    for (int i = 0; i < n; i++) {
        if (_ringCount >= WA_RING_SAMPLES) {
            // drop oldest sample if full
            _ringHead = (_ringHead + 1) % WA_RING_SAMPLES;
            _ringCount--;
        }
        _ring[_ringTail] = pcm[i];
        _ringTail = (_ringTail + 1) % WA_RING_SAMPLES;
        _ringCount++;
    }
    pthread_mutex_unlock(&_ringLock);
}

- (int)popSamples:(int16_t *)out count:(int)want
{
    pthread_mutex_lock(&_ringLock);
    int got = 0;
    while (got < want && _ringCount > 0) {
        out[got++] = _ring[_ringHead];
        _ringHead = (_ringHead + 1) % WA_RING_SAMPLES;
        _ringCount--;
    }
    pthread_mutex_unlock(&_ringLock);
    return got;
}

- (void)playOpusFrame:(NSData *)frame
{
    if (!_dec || !frame.length) return;
    int16_t pcm[WA_FRAME * 6]; // up to 120 ms Opus packet
    int n = opus_decode(_dec, frame.bytes, (opus_int32)frame.length, pcm, WA_FRAME * 6, 0);
    if (n > 0) [self pushSamples:pcm count:n];
}

// Route one received RTP audio payload (MLow TOC in the first byte) to the right decoder.
- (void)playFrame:(NSData *)frame
{
    if (!frame.length) return;
    const uint8_t *b = frame.bytes;
    switch (wa_mlow_route_of(b[0])) {
    case WA_MLOW_OPUS:
        [self playOpusFrame:frame];
        break;
    case WA_MLOW_DTX: {
        // comfort noise / packet-loss fill: one silent frame keeps the clock moving
        int16_t z[WA_FRAME];
        memset(z, 0, sizeof z);
        [self pushSamples:z count:WA_FRAME];
        break;
    }
    case WA_MLOW_SMPL: {
        // MLow (WhatsApp's default inbound codec): decode to 60 ms of float PCM, then push samples.
        float pcm[WA_MLOW_FRAME_SAMPLES];
        int n = wa_mlow_decode_payload(&_mlow, b, (int)frame.length, pcm, WA_MLOW_FRAME_SAMPLES);
        if (n <= 0) {   // -1 => actually a std-Opus frame; fall back to the Opus decoder
            [self playOpusFrame:frame];
            break;
        }
        int16_t out[WA_MLOW_FRAME_SAMPLES];
        for (int i = 0; i < n; i++) {
            float v = pcm[i];
            if (v > 1.0f) v = 1.0f;
            else if (v < -1.0f) v = -1.0f;
            out[i] = (int16_t)(v * 32767.0f);
        }
        [self pushSamples:out count:n];
        break;
    }
    }
}

// --- RemoteIO / VoiceProcessingIO render & input callbacks ---

static OSStatus wa_render(void *inRefCon, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                          UInt32 bus, UInt32 frames, AudioBufferList *io)
{
    WAAudioEngine *self = (__bridge WAAudioEngine *)inRefCon;
    int16_t *out = io && io->mNumberBuffers ? (int16_t *)io->mBuffers[0].mData : NULL;
    if (out && frames > 0) {
        int got = [self popSamples:out count:(int)frames];
        if (got < (int)frames) {
            memset(out + got, 0, (frames - (UInt32)got) * sizeof(int16_t));
        }
    }
    (void)flags; (void)ts; (void)bus;
    return noErr;
}

static OSStatus wa_input(void *inRefCon, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                         UInt32 bus, UInt32 frames, AudioBufferList *io)
{
    WAAudioEngine *self = (__bridge WAAudioEngine *)inRefCon;
    (void)io;
    AudioUnit unit = self->_unit;
    if (!unit) return noErr;
    int16_t buf[2048];
    if (frames > 2048) frames = 2048;
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = 1;
    abl.mBuffers[0].mDataByteSize = frames * sizeof(int16_t);
    abl.mBuffers[0].mData = buf;
    OSStatus st = AudioUnitRender(unit, flags, ts, bus, frames, &abl);
    if (st != noErr) return st;
    if (self.muted || !self.onFrame || !self->_enc) return noErr;

    UInt32 idx = 0;
    while (idx < frames) {
        UInt32 need = (UInt32)(WA_FRAME - self->_capFill);
        UInt32 avail = frames - idx;
        UInt32 take = avail < need ? avail : need;
        memcpy(self->_capBuf + self->_capFill, buf + idx, take * sizeof(int16_t));
        self->_capFill += (int)take;
        idx += take;
        if (self->_capFill == WA_FRAME) {
            self->_capFill = 0;
            uint8_t enc[400];
            int n = opus_encode(self->_enc, self->_capBuf, WA_FRAME, enc, sizeof enc);
            if (n > 0) {
                NSData *d = [NSData dataWithBytes:enc length:(NSUInteger)n];
                self.onFrame(d);
            }
        }
    }
    return noErr;
}

- (BOOL)start
{
    if (_running) return YES;
    _capFill = 0;
    pthread_mutex_lock(&_ringLock);
    _ringHead = _ringTail = _ringCount = 0;
    pthread_mutex_unlock(&_ringLock);

    NSError *err = nil;
    AVAudioSession *sess = [AVAudioSession sharedInstance];
    [sess setCategory:AVAudioSessionCategoryPlayAndRecord error:&err];
    [sess setPreferredSampleRate:WA_SR error:NULL];
    NSTimeInterval bufDur = 0.020;
    [sess setPreferredIOBufferDuration:bufDur error:NULL];
    [sess setActive:YES error:&err];

    BOOL isPad = (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad);
    AudioComponentDescription desc;
    desc.componentType = kAudioUnitType_Output;
    // On iPad iOS 6, VoiceProcessingIO fails to load the (UL-)EQ aupreset in mediaserverd; use RemoteIO on iPad.
    desc.componentSubType = isPad ? kAudioUnitSubType_RemoteIO : kAudioUnitSubType_VoiceProcessingIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &_unit) != noErr) {
        desc.componentSubType = kAudioUnitSubType_RemoteIO;
        comp = AudioComponentFindNext(NULL, &desc);
        if (!comp || AudioComponentInstanceNew(comp, &_unit) != noErr) {
            WALog(@"audio: no RemoteIO/VoiceProcessingIO");
            return NO;
        }
    }

    UInt32 one = 1;
    AudioUnitSetProperty(_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof one);
    AudioUnitSetProperty(_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &one, sizeof one);

    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.mSampleRate = WA_SR;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBytesPerPacket = 2;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 2;
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 16;
    AudioUnitSetProperty(_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &fmt, sizeof fmt);
    AudioUnitSetProperty(_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt);

    AURenderCallbackStruct play = { wa_render, (__bridge void *)self };
    AudioUnitSetProperty(_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &play, sizeof play);
    AURenderCallbackStruct capture = { wa_input, (__bridge void *)self };
    AudioUnitSetProperty(_unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 1, &capture, sizeof capture);

    if (AudioUnitInitialize(_unit) != noErr || AudioOutputUnitStart(_unit) != noErr) {
        WALog(@"audio: start failed");
        AudioComponentInstanceDispose(_unit);
        _unit = NULL;
        return NO;
    }
    _running = YES;
    WALog(@"audio engine started (16 kHz %s + Opus)", isPad ? "RemoteIO" : "VoiceProcessingIO");
    return YES;
}

- (void)setSpeaker:(BOOL)speaker
{
    _speaker = speaker;
    NSError *err = nil;
    AVAudioSessionPortOverride port = speaker ? AVAudioSessionPortOverrideSpeaker : AVAudioSessionPortOverrideNone;
    [[AVAudioSession sharedInstance] overrideOutputAudioPort:port error:&err];
    if (err) WALog(@"audio: setSpeaker error %@", err);
}

- (void)stop
{
    if (!_running) return;
    _running = NO;
    if (_unit) {
        AudioOutputUnitStop(_unit);
        AudioUnitUninitialize(_unit);
        AudioComponentInstanceDispose(_unit);
        _unit = NULL;
    }
}

@end
