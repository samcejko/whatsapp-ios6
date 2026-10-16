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
#define WA_JITTER_FRAMES 16 // ~320 ms of 20 ms frames

@implementation WAAudioEngine {
    AudioUnit _unit;
    OpusEncoder *_enc;
    OpusDecoder *_dec;
    // playback jitter ring of PCM frames
    int16_t _ring[WA_JITTER_FRAMES][WA_FRAME];
    int _ringHead, _ringTail;
    pthread_mutex_t _ringLock;
    BOOL _running;
}

- (instancetype)init
{
    if ((self = [super init])) {
        pthread_mutex_init(&_ringLock, NULL);
        int err = 0;
        _enc = opus_encoder_create(WA_SR, 1, OPUS_APPLICATION_VOIP, &err);
        _dec = opus_decoder_create(WA_SR, 1, &err);
        if (_enc) {
            opus_encoder_ctl(_enc, OPUS_SET_BITRATE(20000));
            opus_encoder_ctl(_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
            opus_encoder_ctl(_enc, OPUS_SET_DTX(1));
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

- (void)pushPCM:(const int16_t *)pcm
{
    pthread_mutex_lock(&_ringLock);
    int next = (_ringTail + 1) % WA_JITTER_FRAMES;
    if (next != _ringHead) { memcpy(_ring[_ringTail], pcm, WA_FRAME * sizeof(int16_t)); _ringTail = next; }
    pthread_mutex_unlock(&_ringLock);
}

- (BOOL)popPCM:(int16_t *)out
{
    pthread_mutex_lock(&_ringLock);
    BOOL have = _ringHead != _ringTail;
    if (have) { memcpy(out, _ring[_ringHead], WA_FRAME * sizeof(int16_t)); _ringHead = (_ringHead + 1) % WA_JITTER_FRAMES; }
    pthread_mutex_unlock(&_ringLock);
    return have;
}

- (void)playOpusFrame:(NSData *)frame
{
    if (!_dec || !frame.length) return;
    int16_t pcm[WA_FRAME];
    int n = opus_decode(_dec, frame.bytes, (opus_int32)frame.length, pcm, WA_FRAME, 0);
    if (n == WA_FRAME) [self pushPCM:pcm];
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
        [self pushPCM:z];
        break;
    }
    case WA_MLOW_SMPL: {
        // MLow smpl DSP decode is the staged port in core/wa_mlow*; until it lands, fill to hold timing
        int16_t z[WA_FRAME];
        memset(z, 0, sizeof z);
        [self pushPCM:z];
        break;
    }
    }
}

// --- the single VoiceProcessingIO render callback handles both capture and playback ---

static OSStatus wa_render(void *inRefCon, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                          UInt32 bus, UInt32 frames, AudioBufferList *io)
{
    WAAudioEngine *self = (__bridge WAAudioEngine *)inRefCon;
    // playback bus (0): fill from the jitter ring
    int16_t pcm[WA_FRAME];
    int16_t *out = io && io->mNumberBuffers ? (int16_t *)io->mBuffers[0].mData : NULL;
    UInt32 want = frames;
    if (out) {
        UInt32 filled = 0;
        while (filled + WA_FRAME <= want && [self popPCM:pcm]) { memcpy(out + filled, pcm, WA_FRAME * sizeof(int16_t)); filled += WA_FRAME; }
        if (filled < want) memset(out + filled, 0, (want - filled) * sizeof(int16_t));
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
    // encode in 20 ms chunks
    for (UInt32 off = 0; off + WA_FRAME <= frames; off += WA_FRAME) {
        uint8_t enc[400];
        int n = opus_encode(self->_enc, buf + off, WA_FRAME, enc, sizeof enc);
        if (n > 0) {
            NSData *d = [NSData dataWithBytes:enc length:n];
            self.onFrame(d);
        }
    }
    return noErr;
}

- (BOOL)start
{
    if (_running) return YES;

    NSError *err = nil;
    AVAudioSession *sess = [AVAudioSession sharedInstance];
    [sess setCategory:AVAudioSessionCategoryPlayAndRecord error:&err];
    [sess setPreferredSampleRate:WA_SR error:NULL];
    [sess setActive:YES error:&err];

    AudioComponentDescription desc;
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;   // echo cancellation
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &_unit) != noErr) { WALog(@"audio: no VoiceProcessingIO"); return NO; }

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

    if (AudioUnitInitialize(_unit) != noErr || AudioOutputUnitStart(_unit) != noErr) { WALog(@"audio: start failed"); return NO; }
    _running = YES;
    WALog(@"audio engine started (16 kHz VoiceProcessingIO + Opus)");
    return YES;
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
