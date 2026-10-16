#import "WAChatStore.h"
#import "WACommon.h"
#import "WASession.h"
#include "wa_history.h"   // WA_MSG_* kinds

NSString *const WAChatStoreChanged = @"WAChatStoreChanged";

#define WA_MAX_MSGS_PER_CHAT 2000

@interface WAChatStore ()
@property (nonatomic, strong) NSMutableDictionary *chats;   // canonicalKey -> mutable @{ jid, name, ts, unread, msgs }
@end

@implementation WAChatStore

+ (instancetype)shared
{
    static WAChatStore *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[WAChatStore alloc] init]; });
    return s;
}

- (instancetype)init
{
    if ((self = [super init])) {
        [self load];
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        [nc addObserver:self selector:@selector(onConversations:) name:WASessionConversations object:nil];
        [nc addObserver:self selector:@selector(onMessage:) name:WASessionMessage object:nil];
    }
    return self;
}

- (NSString *)path { return [WADocumentsPath() stringByAppendingPathComponent:@"wa6.chats.plist"]; }

// Canonical key for a JID: the digits of the user part (ignores the :device suffix and server), so a conversation's
// history id and the bare JID of a live message from the same number land in the same chat. Falls back to the whole
// string when there are no digits (defensive).
- (NSString *)keyFor:(NSString *)jid
{
    if (!jid.length) return @"";
    NSString *user = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
    NSString *digits = [[user componentsSeparatedByCharactersInSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]] componentsJoinedByString:@""];
    return digits.length ? digits : jid;
}

- (void)load
{
    self.chats = [NSMutableDictionary dictionary];
    NSDictionary *disk = [NSDictionary dictionaryWithContentsOfFile:[self path]];
    for (NSString *k in disk) {
        NSDictionary *c = disk[k];
        if (![c isKindOfClass:[NSDictionary class]]) continue;
        NSMutableDictionary *entry = [c mutableCopy];
        NSArray *msgs = c[@"msgs"];
        entry[@"msgs"] = [([msgs isKindOfClass:[NSArray class]] ? msgs : @[]) mutableCopy];
        self.chats[k] = entry;
    }
}

- (void)save
{
    // writeToFile wants a plain property list; the nested mutable containers are all plist types already.
    if (![self.chats writeToFile:[self path] atomically:YES]) WALog(@"chat store: save failed");
}

- (NSMutableDictionary *)entryForJID:(NSString *)jid create:(BOOL)create
{
    NSString *key = [self keyFor:jid];
    NSMutableDictionary *e = self.chats[key];
    if (!e && create) {
        e = [NSMutableDictionary dictionary];
        e[@"jid"] = jid ?: @"";
        e[@"name"] = @"";
        e[@"ts"] = @0;
        e[@"unread"] = @0;
        e[@"msgs"] = [NSMutableArray array];
        self.chats[key] = e;
    }
    return e;
}

// A signature used to avoid storing the same message twice (id when present, else timestamp+direction+text).
static NSString *msgSig(NSDictionary *m)
{
    NSString *mid = m[@"id"];
    if ([mid isKindOfClass:[NSString class]] && mid.length) return [@"id:" stringByAppendingString:mid];
    return [NSString stringWithFormat:@"%@|%@|%@", m[@"ts"] ?: @0, m[@"out"] ?: @NO, m[@"text"] ?: @""];
}

// Merge a batch of messages (dicts @{text,out,ts,id}) into a chat entry, de-duplicating and keeping oldest-first.
- (void)mergeMessages:(NSArray *)incoming into:(NSMutableDictionary *)entry
{
    NSMutableArray *msgs = entry[@"msgs"];
    NSMutableSet *seen = [NSMutableSet set];
    for (NSDictionary *m in msgs) [seen addObject:msgSig(m)];
    BOOL added = NO;
    for (NSDictionary *m in incoming) {
        NSString *sig = msgSig(m);
        if ([seen containsObject:sig]) continue;
        [seen addObject:sig];
        [msgs addObject:m];
        added = YES;
    }
    if (added) {
        [msgs sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
            unsigned long long ta = [a[@"ts"] unsignedLongLongValue], tb = [b[@"ts"] unsignedLongLongValue];
            if (ta == tb) return NSOrderedSame;
            return ta < tb ? NSOrderedAscending : NSOrderedDescending;   // oldest-first
        }];
        if (msgs.count > WA_MAX_MSGS_PER_CHAT)
            [msgs removeObjectsInRange:NSMakeRange(0, msgs.count - WA_MAX_MSGS_PER_CHAT)];   // keep the newest tail
    }
}

- (void)onConversations:(NSNotification *)n
{
    NSArray *convs = n.userInfo[@"conversations"];
    if (![convs isKindOfClass:[NSArray class]]) return;
    for (NSDictionary *c in convs) {
        NSString *jid = c[@"jid"];
        if (![jid isKindOfClass:[NSString class]] || !jid.length) continue;
        NSMutableDictionary *e = [self entryForJID:jid create:YES];
        NSString *name = c[@"name"];
        if ([name isKindOfClass:[NSString class]] && name.length) e[@"name"] = name;
        unsigned long long ts = [c[@"ts"] unsignedLongLongValue];
        if (ts > [e[@"ts"] unsignedLongLongValue]) e[@"ts"] = @(ts);
        if (c[@"unread"]) e[@"unread"] = c[@"unread"];
        NSArray *msgs = c[@"messages"];
        if ([msgs isKindOfClass:[NSArray class]] && msgs.count) {
            [self mergeMessages:msgs into:e];
            // keep the chat's ts at least as recent as its newest message
            unsigned long long last = [[[e[@"msgs"] lastObject] objectForKey:@"ts"] unsignedLongLongValue];
            if (last > [e[@"ts"] unsignedLongLongValue]) e[@"ts"] = @(last);
        }
    }
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:nil];
}

- (void)onMessage:(NSNotification *)n
{
    NSString *from = n.userInfo[@"from"];
    NSString *text = n.userInfo[@"text"];
    if (![from isKindOfClass:[NSString class]] || !from.length) return;
    [self appendText:(text ?: @"") toJID:from outgoing:NO ts:(unsigned long long)[[NSDate date] timeIntervalSince1970] msgId:nil];
}

- (void)recordOutgoing:(NSString *)text toJID:(NSString *)jid
{
    if (!jid.length) return;
    [self appendText:(text ?: @"") toJID:jid outgoing:YES ts:(unsigned long long)[[NSDate date] timeIntervalSince1970] msgId:nil];
}

- (void)appendText:(NSString *)text toJID:(NSString *)jid outgoing:(BOOL)out ts:(unsigned long long)ts msgId:(NSString *)mid
{
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    if (![e[@"name"] length]) e[@"name"] = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
    NSDictionary *m = @{ @"text": text, @"out": @(out), @"ts": @(ts), @"id": (mid ?: @""), @"kind": @(WA_MSG_TEXT) };
    [self mergeMessages:@[ m ] into:e];
    if (ts > [e[@"ts"] unsignedLongLongValue]) e[@"ts"] = @(ts);
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:jid] }];
}

// A localized "📷 Photo"-style label for a media kind (shared by the list preview and the chat bubbles).
+ (NSString *)mediaLabelForKind:(int)kind
{
    switch (kind) {
        case WA_MSG_IMAGE:    return [@"\U0001F4F7 " stringByAppendingString:WALoc(@"media_photo")];
        case WA_MSG_VIDEO:    return [@"\U0001F3AC " stringByAppendingString:WALoc(@"media_video")];
        case WA_MSG_AUDIO:    return [@"\U0001F3A4 " stringByAppendingString:WALoc(@"media_voice")];
        case WA_MSG_DOCUMENT: return [@"\U0001F4C4 " stringByAppendingString:WALoc(@"media_document")];
        case WA_MSG_STICKER:  return WALoc(@"media_sticker");
        case WA_MSG_LOCATION: return [@"\U0001F4CD " stringByAppendingString:WALoc(@"media_location")];
        case WA_MSG_CONTACT:  return [@"\U0001F464 " stringByAppendingString:WALoc(@"media_contact")];
        default:              return @"";
    }
}

// The one-line preview for the chat list: the text/caption when present, else a media label.
+ (NSString *)previewForMessage:(NSDictionary *)m
{
    NSString *text = m[@"text"];
    if ([text isKindOfClass:[NSString class]] && text.length) return text;
    return [self mediaLabelForKind:[m[@"kind"] intValue]];
}

- (void)deleteChat:(NSString *)jid
{
    NSString *key = [self keyFor:jid];
    if (!self.chats[key]) return;
    [self.chats removeObjectForKey:key];
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:nil];
}

- (NSArray *)conversationsSorted
{
    NSMutableArray *rows = [NSMutableArray arrayWithCapacity:self.chats.count];
    for (NSString *k in self.chats) {
        NSDictionary *e = self.chats[k];
        NSDictionary *lastMsg = [e[@"msgs"] lastObject];
        NSString *last = lastMsg ? [WAChatStore previewForMessage:lastMsg] : @"";
        [rows addObject:@{ @"jid": e[@"jid"] ?: @"",
                           @"name": e[@"name"] ?: @"",
                           @"last": last,
                           @"ts": e[@"ts"] ?: @0,
                           @"unread": e[@"unread"] ?: @0 }];
    }
    [rows sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        unsigned long long ta = [a[@"ts"] unsignedLongLongValue], tb = [b[@"ts"] unsignedLongLongValue];
        if (ta == tb) return NSOrderedSame;
        return ta > tb ? NSOrderedAscending : NSOrderedDescending;   // newest-first
    }];
    return rows;
}

- (NSArray *)messagesForJID:(NSString *)jid
{
    NSDictionary *e = self.chats[[self keyFor:jid]];
    NSArray *msgs = e[@"msgs"];
    return [msgs isKindOfClass:[NSArray class]] ? msgs : @[];
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

@end
