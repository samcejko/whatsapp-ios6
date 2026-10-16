#import "WAAudioPlayer.h"
#import "WACommon.h"
#import <AudioToolbox/AudioToolbox.h>
#include "opus.h"
#include "wa_history.h"
#include "wa_util.h"

NSString *const WAAudioPlayerStateChanged = @"WAAudioPlayerStateChanged";

@interface WAAudioPlayer () <AVAudioPlayerDelegate>
@property (nonatomic, strong) AVAudioPlayer *player;
@property (nonatomic, copy) NSString *currentMsgId;
@property (nonatomic, strong) NSTimer *progressTimer;
@property (nonatomic) BOOL isPlaying;
@property (nonatomic) BOOL isPaused;
@end

// Decodes Ogg Opus stream to 16 kHz 16-bit mono PCM WAV.
// If data is already WAV, MP3, or AAC/M4A, returns it as-is.
static NSData *decodeAudioToPlayable(const uint8_t *raw, size_t len, NSString **outExt)
{
    if (len < 4) return nil;

    // Detect format
    if (len >= 12 && memcmp(raw, "RIFF", 4) == 0 && memcmp(raw + 8, "WAVE", 4) == 0) {
        if (outExt) *outExt = @"wav";
        return [NSData dataWithBytes:raw length:len];
    }
    if ((len >= 3 && memcmp(raw, "ID3", 3) == 0) || (len >= 2 && raw[0] == 0xFF && (raw[1] & 0xFE) == 0xFA)) {
        if (outExt) *outExt = @"mp3";
        return [NSData dataWithBytes:raw length:len];
    }
    if (len >= 8 && (memcmp(raw + 4, "ftyp", 4) == 0 || memcmp(raw, "\x00\x00\x00", 3) == 0)) {
        if (outExt) *outExt = @"m4a";
        return [NSData dataWithBytes:raw length:len];
    }

    if (len < 27 || memcmp(raw, "OggS", 4) != 0) {
        // Unknown or raw PCM fallback
        return nil;
    }

    // Ogg Opus parsing and decoding
    int err = 0;
    OpusDecoder *dec = opus_decoder_create(16000, 1, &err);
    if (!dec) return nil;

    NSMutableData *pcmData = [NSMutableData data];
    NSMutableData *curPkt = [NSMutableData data];
    size_t pos = 0;
    int packetIdx = 0;

    while (pos + 27 <= len) {
        if (memcmp(raw + pos, "OggS", 4) != 0) {
            pos++;
            continue;
        }
        uint8_t nsegs = raw[pos + 26];
        if (pos + 27 + nsegs > len) break;
        const uint8_t *segtable = raw + pos + 27;
        size_t page_payload = 0;
        for (int i = 0; i < nsegs; i++) page_payload += segtable[i];
        size_t payload_offset = pos + 27 + nsegs;
        if (payload_offset + page_payload > len) break;

        size_t seg_offset = payload_offset;
        for (int i = 0; i < nsegs; ) {
            size_t chunk_len = 0;
            BOOL pkt_ends = NO;
            while (i < nsegs) {
                chunk_len += segtable[i];
                if (segtable[i] < 255) {
                    i++;
                    pkt_ends = YES;
                    break;
                }
                i++;
            }
            const uint8_t *chunk = raw + seg_offset;
            seg_offset += chunk_len;

            [curPkt appendBytes:chunk length:chunk_len];
            if (pkt_ends) {
                packetIdx++;
                // Skip OpusHead (packet 1) and OpusTags (packet 2)
                if (packetIdx > 2 && curPkt.length > 0) {
                    int16_t out_pcm[5760];
                    int samples = opus_decode(dec, (const unsigned char *)curPkt.bytes, (opus_int32)curPkt.length, out_pcm, 5760, 0);
                    if (samples > 0) {
                        [pcmData appendBytes:out_pcm length:samples * sizeof(int16_t)];
                    }
                }
                [curPkt setLength:0];
            }
        }
        pos = payload_offset + page_payload;
    }
    opus_decoder_destroy(dec);

    if (pcmData.length == 0) return nil;

    // Build 44-byte WAV header
    uint32_t sample_rate = 16000;
    uint16_t channels = 1;
    uint16_t bits = 16;
    uint32_t data_size = (uint32_t)pcmData.length;
    uint32_t riff_size = 36 + data_size;
    uint32_t byte_rate = sample_rate * channels * (bits / 8);
    uint16_t block_align = channels * (bits / 8);

    uint8_t hdr[44];
    memcpy(hdr + 0, "RIFF", 4);
    hdr[4] = (uint8_t)(riff_size);
    hdr[5] = (uint8_t)(riff_size >> 8);
    hdr[6] = (uint8_t)(riff_size >> 16);
    hdr[7] = (uint8_t)(riff_size >> 24);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;
    hdr[20] = 1;  hdr[21] = 0;
    hdr[22] = (uint8_t)channels; hdr[23] = 0;
    hdr[24] = (uint8_t)(sample_rate);
    hdr[25] = (uint8_t)(sample_rate >> 8);
    hdr[26] = (uint8_t)(sample_rate >> 16);
    hdr[27] = (uint8_t)(sample_rate >> 24);
    hdr[28] = (uint8_t)(byte_rate);
    hdr[29] = (uint8_t)(byte_rate >> 8);
    hdr[30] = (uint8_t)(byte_rate >> 16);
    hdr[31] = (uint8_t)(byte_rate >> 24);
    hdr[32] = (uint8_t)block_align; hdr[33] = 0;
    hdr[34] = (uint8_t)bits; hdr[35] = 0;
    memcpy(hdr + 36, "data", 4);
    hdr[40] = (uint8_t)(data_size);
    hdr[41] = (uint8_t)(data_size >> 8);
    hdr[42] = (uint8_t)(data_size >> 16);
    hdr[43] = (uint8_t)(data_size >> 24);

    NSMutableData *wav = [NSMutableData dataWithBytes:hdr length:sizeof(hdr)];
    [wav appendData:pcmData];
    if (outExt) *outExt = @"wav";
    return wav;
}

@implementation WAAudioPlayer

+ (instancetype)shared
{
    static WAAudioPlayer *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[WAAudioPlayer alloc] init]; });
    return s;
}

- (NSTimeInterval)currentTime
{
    return self.player ? self.player.currentTime : 0;
}

- (NSTimeInterval)duration
{
    return self.player ? self.player.duration : 0;
}

- (void)startProgressTimer
{
    [self.progressTimer invalidate];
    self.progressTimer = [NSTimer scheduledTimerWithTimeInterval:0.25 target:self selector:@selector(onTimer:) userInfo:nil repeats:YES];
}

- (void)stopProgressTimer
{
    [self.progressTimer invalidate];
    self.progressTimer = nil;
}

- (void)onTimer:(NSTimer *)t
{
    if (self.isPlaying) {
        [[NSNotificationCenter defaultCenter] postNotificationName:WAAudioPlayerStateChanged object:self];
    }
}

- (void)startPlayingURL:(NSURL *)url msgId:(NSString *)mid
{
    NSError *err = nil;
    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayback error:&err];
    [session setActive:YES error:&err];

    [self.player stop];
    self.player = [[AVAudioPlayer alloc] initWithContentsOfURL:url error:&err];
    if (err || !self.player) {
        WALog(@"WAAudioPlayer init error: %@", err);
        self.isPlaying = NO;
        self.isPaused = NO;
        self.currentMsgId = nil;
        [self stopProgressTimer];
        [[NSNotificationCenter defaultCenter] postNotificationName:WAAudioPlayerStateChanged object:self];
        return;
    }
    self.player.delegate = self;
    [self.player prepareToPlay];
    if ([self.player play]) {
        self.isPlaying = YES;
        self.isPaused = NO;
        self.currentMsgId = mid;
        [self startProgressTimer];
    } else {
        self.isPlaying = NO;
        self.isPaused = NO;
        self.currentMsgId = nil;
        [self stopProgressTimer];
    }
    [[NSNotificationCenter defaultCenter] postNotificationName:WAAudioPlayerStateChanged object:self];
}

- (void)playOrToggleMessage:(NSDictionary *)msg
{
    NSString *mid = msg[@"id"];
    if (!mid.length) mid = [NSString stringWithFormat:@"ts_%@", msg[@"ts"] ?: @(wa_now_us())];

    if ([self.currentMsgId isEqualToString:mid]) {
        if (self.isPlaying) {
            [self pause];
        } else if (self.isPaused) {
            [self.player play];
            self.isPlaying = YES;
            self.isPaused = NO;
            [self startProgressTimer];
            [[NSNotificationCenter defaultCenter] postNotificationName:WAAudioPlayerStateChanged object:self];
        }
        return;
    }

    [self stop];

    // Check Documents cache
    NSString *basePath = [WADocumentsPath() stringByAppendingPathComponent:[NSString stringWithFormat:@"wa6_aud_%@", mid]];
    NSFileManager *fm = [NSFileManager defaultManager];
    for (NSString *ext in @[ @"wav", @"mp3", @"m4a" ]) {
        NSString *p = [basePath stringByAppendingPathExtension:ext];
        if ([fm fileExistsAtPath:p]) {
            [self startPlayingURL:[NSURL fileURLWithPath:p] msgId:mid];
            return;
        }
    }

    // Check inline audio data
    NSData *inlineAudio = msg[@"audio_data"];
    if ([inlineAudio isKindOfClass:[NSData class]] && inlineAudio.length > 0) {
        NSString *ext = @"wav";
        NSData *playable = decodeAudioToPlayable(inlineAudio.bytes, inlineAudio.length, &ext);
        if (playable.length) {
            NSString *p = [basePath stringByAppendingPathExtension:ext];
            [playable writeToFile:p atomically:YES];
            [self startPlayingURL:[NSURL fileURLWithPath:p] msgId:mid];
            return;
        }
    }

    // Download from CDN
    NSString *dpath = msg[@"dpath"];
    NSData *mkey = msg[@"mkey"];
    NSData *esha = msg[@"esha"];
    if (![dpath isKindOfClass:[NSString class]] || ![mkey isKindOfClass:[NSData class]] || mkey.length != 32) {
        WALog(@"WAAudioPlayer: no audio refs to download for %@", mid);
        return;
    }

    const char *cdpath = strdup(dpath.UTF8String);
    NSData *mk = mkey, *es = [esha isKindOfClass:[NSData class]] ? esha : nil;

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        wa_buf out;
        wa_buf_init(&out);
        int rc = wa_media_download(cdpath, mk.bytes, es.length ? es.bytes : NULL, es.length, WA_MSG_AUDIO, NULL, &out);
        free((void *)cdpath);
        if (rc == 0 && out.len > 0) {
            NSString *ext = @"wav";
            NSData *playable = decodeAudioToPlayable(out.data, out.len, &ext);
            wa_buf_free(&out);
            if (playable.length) {
                NSString *p = [basePath stringByAppendingPathExtension:ext];
                [playable writeToFile:p atomically:YES];
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self startPlayingURL:[NSURL fileURLWithPath:p] msgId:mid];
                });
                return;
            }
        } else {
            wa_buf_free(&out);
        }
    });
}

- (void)pause
{
    if (self.player && self.isPlaying) {
        [self.player pause];
        self.isPlaying = NO;
        self.isPaused = YES;
        [self stopProgressTimer];
        [[NSNotificationCenter defaultCenter] postNotificationName:WAAudioPlayerStateChanged object:self];
    }
}

- (void)stop
{
    if (self.player) {
        [self.player stop];
        self.player = nil;
    }
    self.isPlaying = NO;
    self.isPaused = NO;
    self.currentMsgId = nil;
    [self stopProgressTimer];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAAudioPlayerStateChanged object:self];
}

#pragma mark - AVAudioPlayerDelegate

- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer *)player successfully:(BOOL)flag
{
    self.isPlaying = NO;
    self.isPaused = NO;
    self.currentMsgId = nil;
    [self stopProgressTimer];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAAudioPlayerStateChanged object:self];
}

- (void)audioPlayerDecodeErrorDidOccur:(AVAudioPlayer *)player error:(NSError *)error
{
    WALog(@"WAAudioPlayer decode error: %@", error);
    [self stop];
}

- (void)dealloc
{
    [self stop];
}

@end
