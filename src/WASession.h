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
extern NSString *const WASessionIncomingCall;    // userInfo: @"jid" (NSString), @"video" (NSNumber BOOL), @"callId" (NSString)
extern NSString *const WASessionCallRinging;     // userInfo: @"callId" (NSString)
extern NSString *const WASessionCallAccepted;    // userInfo: @"callId" (NSString)
extern NSString *const WASessionCallEnded;       // userInfo: @"callId" (NSString), @"reason" (NSString, optional)
extern NSString *const WASessionCallAudioPacket; // userInfo: @"payload" (NSData)

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
@property (nonatomic, readonly) BOOL syncing;               // Currently syncing messages (for UI indicator)

- (void)start;
- (void)stop;
- (void)reconnect;         // Manually reconnect (for sync button)
- (void)unlinkAndReset;    // forget the device identity (local only) and re-pair

// Send a 1:1 text to a bare user JID ("<num>@s.whatsapp.net"). Runs on the client thread.
- (void)sendText:(NSString *)text toJID:(NSString *)jid;
- (void)sendText:(NSString *)text toJID:(NSString *)jid msgId:(NSString *)mid;
- (void)sendText:(NSString *)text toJID:(NSString *)jid msgId:(NSString *)mid quoted:(NSDictionary *)quoted;
- (void)sendReaction:(NSString *)emoji toMessageId:(NSString *)mid fromMe:(BOOL)fromMe inChat:(NSString *)jid;
- (void)sendImage:(NSData *)jpegData thumb:(NSData *)thumbData caption:(NSString *)caption toJID:(NSString *)jid msgId:(NSString *)mid;
- (void)sendAudio:(NSData *)audioData duration:(uint32_t)dur toJID:(NSString *)jid msgId:(NSString *)mid;
- (void)sendReadReceiptForMessage:(NSString *)mid toJID:(NSString *)jid participant:(NSString *)part;
// Place a 1:1 call to a bare user JID. Sends the encrypted <offer>; the media comes up on the client thread.
- (void)placeCallTo:(NSString *)jid video:(BOOL)video;
- (void)acceptCall;
- (void)rejectCall;
- (void)terminateCall;
- (void)sendCallAudioFrame:(NSData *)opusFrame;

// Subscribe/unsubscribe to presence updates for a contact JID
- (void)subscribePresence:(NSString *)jid;
- (void)unsubscribePresence:(NSString *)jid;
// Send typing indicator (composing = YES) or idle (composing = NO)
- (void)sendChatState:(BOOL)composing toJID:(NSString *)jid;
// Send audio recording indicator
- (void)sendRecordingState:(BOOL)recording toJID:(NSString *)jid;

// Block / unblock contact
- (void)setBlockContact:(NSString *)jid blocked:(BOOL)blocked;

// Request contact's profile picture preview
- (void)fetchProfilePicture:(NSString *)jid;

// Request group metadata (subject & participants) via w:g2
- (void)fetchGroupMetadata:(NSString *)groupJID;

@end
