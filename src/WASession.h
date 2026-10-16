// WASession bridges the C protocol core (wa_client) to the UI. It owns the device identity and the connection,
// runs the client loop on a background thread, and republishes its events as notifications on the main thread.
#import <Foundation/Foundation.h>

extern NSString *const WASessionStateChanged;   // userInfo: @"state" (NSNumber), @"detail" (NSString, optional)
extern NSString *const WASessionQRChanged;       // userInfo: @"qr" (NSString)
extern NSString *const WASessionPaired;          // userInfo: @"jid"
extern NSString *const WASessionNodeReceived;    // userInfo: @"xml" (NSString, debug render)
extern NSString *const WASessionMessage;         // userInfo: @"from" (bare JID), @"text"
extern NSString *const WASessionMessageStatus;   // userInfo: @"id" (NSString), @"status" (NSNumber), @"error" (NSString, optional)
extern NSString *const WASessionConversations;   // userInfo: @"conversations" (NSArray of @{jid,name,last,ts,unread}) from a history-sync chunk
extern NSString *const WASessionPresenceChanged; // userInfo: @"jid" (NSString), @"state" (NSString: available/unavailable/composing/paused), @"last" (NSString, optional)

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

// Send a 1:1 text to a bare user JID ("<num>@s.whatsapp.net"). Runs on the client thread.
- (void)sendText:(NSString *)text toJID:(NSString *)jid;
- (void)sendText:(NSString *)text toJID:(NSString *)jid msgId:(NSString *)mid;
// Place a 1:1 call to a bare user JID. Sends the encrypted <offer>; the media comes up on the client thread.
- (void)placeCallTo:(NSString *)jid video:(BOOL)video;

// Subscribe/unsubscribe to presence updates for a contact JID
- (void)subscribePresence:(NSString *)jid;
- (void)unsubscribePresence:(NSString *)jid;
// Send typing indicator (composing = YES) or idle (composing = NO)
- (void)sendChatState:(BOOL)composing toJID:(NSString *)jid;

// Request contact's profile picture preview
- (void)fetchProfilePicture:(NSString *)jid;

@end
