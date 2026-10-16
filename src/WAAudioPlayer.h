// WAAudioPlayer: plays voice notes and audio messages (WA_MSG_AUDIO).
// Handles WhatsApp Ogg Opus decoding into 16 kHz 16-bit PCM WAV, cached to Documents,
// and plays through AVAudioPlayer with speaker routing.
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>

extern NSString *const WAAudioPlayerStateChanged;

@interface WAAudioPlayer : NSObject

+ (instancetype)shared;

@property (nonatomic, readonly) BOOL isPlaying;
@property (nonatomic, readonly) BOOL isPaused;
@property (nonatomic, readonly, copy) NSString *currentMsgId;
@property (nonatomic, readonly) NSTimeInterval currentTime;
@property (nonatomic, readonly) NSTimeInterval duration;

// Play or toggle play/pause for an audio message dict from WAChatStore
- (void)playOrToggleMessage:(NSDictionary *)msg;
- (void)pause;
- (void)stop;

@end
