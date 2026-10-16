// Persistent chat history for Wazzup. Holds, per conversation, its display name and its messages, saved to a plist
// in Documents so the chat list and each chat's bubbles survive relaunches. It is the single source of truth the UI
// reads: it ingests history-sync conversations and live incoming messages (observed) and records outgoing ones, then
// posts WAChatStoreChanged so the open view controllers reload.
#import <Foundation/Foundation.h>

extern NSString *const WAChatStoreChanged;   // userInfo may carry @"jid" (canonical) when a single chat changed

typedef NS_ENUM(NSInteger, WAMessageStatus) {
    WAMessageStatusError = 0,
    WAMessageStatusPending = 1,
    WAMessageStatusSent = 2,
    WAMessageStatusDelivered = 3,
    WAMessageStatusRead = 4,
    WAMessageStatusPlayed = 5,
};

@interface WAChatStore : NSObject

+ (instancetype)shared;

// Canonical key for a JID: the digits of the user part
+ (NSString *)keyForJID:(NSString *)jid;

// Chat list rows, newest-first: array of @{ @"jid", @"name", @"last", @"ts", @"unread" }.
- (NSArray *)conversationsSorted;
// Messages for a conversation, oldest-first: array of @{ @"text", @"out" (NSNumber BOOL), @"ts" }.
- (NSArray *)messagesForJID:(NSString *)jid;

// Record a message the user just sent (persist + notify).
- (void)recordOutgoing:(NSString *)text toJID:(NSString *)jid;
- (void)recordOutgoing:(NSString *)text toJID:(NSString *)jid msgId:(NSString *)mid status:(WAMessageStatus)status;
- (void)recordOutgoingMedia:(NSData *)thumbData caption:(NSString *)caption toJID:(NSString *)jid msgId:(NSString *)mid status:(WAMessageStatus)status;

// Update status of a message by id (e.g. pending -> sent -> delivered -> read)
- (void)updateMessageStatus:(WAMessageStatus)status msgId:(NSString *)mid;

// Delete a conversation and its messages (persist + notify).
- (void)deleteChat:(NSString *)jid;

// Re-resolves contact names against AddressBook and push names, persists if changed.
- (void)refreshContactNames;

// Sets/updates the peer's WhatsApp pushName for a JID
- (void)setPushName:(NSString *)pushName forJID:(NSString *)jid;

// A localized "📷 Photo"-style label for a media message kind (WA_MSG_*), shared by the list and the bubbles.
+ (NSString *)mediaLabelForKind:(int)kind;

@end
