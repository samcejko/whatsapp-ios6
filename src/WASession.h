// WASession bridges the C protocol core (wa_client) to the UI. It owns the device identity and the connection,
// runs the client loop on a background thread, and republishes its events as notifications on the main thread.
#import <Foundation/Foundation.h>

extern NSString *const WASessionStateChanged;   // userInfo: @"state" (NSNumber), @"detail" (NSString, optional)
extern NSString *const WASessionQRChanged;       // userInfo: @"qr" (NSString)
extern NSString *const WASessionPaired;          // userInfo: @"jid"
extern NSString *const WASessionNodeReceived;    // userInfo: @"xml" (NSString, debug render)

typedef NS_ENUM(NSInteger, WASessionState) {
    WASessionStateDisconnected = 0,
    WASessionStateConnecting,
    WASessionStateHandshaking,
    WASessionStatePairing,
    WASessionStateConnected,
};

@interface WASession : NSObject

+ (instancetype)shared;

@property (nonatomic, readonly) WASessionState state;
@property (nonatomic, readonly, copy) NSString *qrString;   // current pairing QR, or nil
@property (nonatomic, readonly) BOOL registered;            // paired to an account
@property (nonatomic, readonly, copy) NSString *ownJID;

- (void)start;
- (void)stop;
- (void)unlinkAndReset;   // forget the device identity (local only) and re-pair

@end
