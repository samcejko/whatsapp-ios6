// Persistent chat history for Wazzup. Holds, per conversation, its display name and its messages, saved to a plist
// in Documents so the chat list and each chat's bubbles survive relaunches. It is the single source of truth the UI
// reads: it ingests history-sync conversations and live incoming messages (observed) and records outgoing ones, then
// posts WAChatStoreChanged so the open view controllers reload.
#import <Foundation/Foundation.h>

extern NSString *const WAChatStoreChanged;   // userInfo may carry @"jid" (canonical) when a single chat changed

@interface WAChatStore : NSObject

+ (instancetype)shared;

// Chat list rows, newest-first: array of @{ @"jid", @"name", @"last", @"ts", @"unread" }.
- (NSArray *)conversationsSorted;
// Messages for a conversation, oldest-first: array of @{ @"text", @"out" (NSNumber BOOL), @"ts" }.
- (NSArray *)messagesForJID:(NSString *)jid;

// Record a message the user just sent (persist + notify).
- (void)recordOutgoing:(NSString *)text toJID:(NSString *)jid;

@end
