// The call audio engine (iOS): VoiceProcessingIO (echo cancellation) at 16 kHz mono, Opus encode of the mic and
// Opus decode of received frames into a small jitter buffer for playback. Outgoing encoded frames are handed to
// the onFrame block (the call engine SRTP-wraps and sends them); received frames are fed in with -playFrame:.
// (MLow is WhatsApp's default inbound codec; decoding it is a follow-up — Opus frames play today.)
#import <Foundation/Foundation.h>

@interface WAAudioEngine : NSObject

// Called on the audio thread with one encoded Opus frame (20 ms, 16 kHz) captured from the mic.
@property (nonatomic, copy) void (^onFrame)(NSData *opusFrame);
@property (nonatomic) BOOL muted;

- (BOOL)start;
- (void)stop;
// Feed one received encoded frame (Opus) for playback.
- (void)playOpusFrame:(NSData *)frame;
// Feed one received RTP audio payload (routed by its MLow TOC byte: Opus / DTX / MLow).
- (void)playFrame:(NSData *)frame;

@end
