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
        [nc addObserver:self selector:@selector(onMessageStatus:) name:WASessionMessageStatus object:nil];
    }
    return self;
}

- (NSString *)path { return [WADocumentsPath() stringByAppendingPathComponent:@"wa6.chats.plist"]; }

// Canonical key for a JID: the digits of the user part (ignores the :device suffix and server), so a conversation's
// history id and the bare JID of a live message from the same number land in the same chat. Falls back to the whole
// string when there are no digits (defensive).
+ (NSString *)keyForJID:(NSString *)jid
{
    if (!jid.length) return @"";
    NSString *user = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
    NSString *digits = [[user componentsSeparatedByCharactersInSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]] componentsJoinedByString:@""];
    return digits.length ? digits : jid;
}

- (NSString *)keyFor:(NSString *)jid
{
    return [WAChatStore keyForJID:jid];
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
        NSMutableArray *mutMsgs = [NSMutableArray array];
        if ([msgs isKindOfClass:[NSArray class]]) {
            for (id m in msgs) {
                if ([m isKindOfClass:[NSDictionary class]]) [mutMsgs addObject:[m mutableCopy]];
            }
        }
        entry[@"msgs"] = mutMsgs;
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
        e[@"name"] = WADisplayNameForChat(jid, nil);
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

// Merge a batch of messages into a chat entry, de-duplicating and keeping oldest-first.
- (void)mergeMessages:(NSArray *)incoming into:(NSMutableDictionary *)entry
{
    NSMutableArray *msgs = entry[@"msgs"];
    NSMutableSet *seen = [NSMutableSet set];
    for (NSDictionary *m in msgs) [seen addObject:msgSig(m)];
    BOOL added = NO;
    for (NSDictionary *m in incoming) {
        NSString *sig = msgSig(m);
        if ([seen containsObject:sig]) {
            NSString *mid = m[@"id"];
            if ([mid isKindOfClass:[NSString class]] && mid.length && m[@"status"]) {
                for (NSMutableDictionary *existing in msgs) {
                    if ([existing[@"id"] isEqualToString:mid]) {
                        int cur = [existing[@"status"] intValue];
                        int inc = [m[@"status"] intValue];
                        if (inc == WAMessageStatusError || inc > cur) {
                            existing[@"status"] = @(inc);
                            added = YES;
                        }
                        break;
                    }
                }
            }
            continue;
        }
        [seen addObject:sig];
        NSMutableDictionary *mut = [m isKindOfClass:[NSMutableDictionary class]] ? m : [m mutableCopy];
        [msgs addObject:mut];
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
        if ([name isKindOfClass:[NSString class]] && name.length && !WAIsRawNumberOrEmpty(name)) {
            e[@"name"] = name;
        } else if (![e[@"name"] length] || WAIsRawNumberOrEmpty(e[@"name"])) {
            e[@"name"] = WADisplayNameForChat(jid, name);
        }
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
    if (![from isKindOfClass:[NSString class]] || !from.length) return;
    [self appendLiveMessage:n.userInfo toJID:from];
}

- (void)appendLiveMessage:(NSDictionary *)msg toJID:(NSString *)jid
{
    if (!jid.length || ![msg isKindOfClass:[NSDictionary class]]) return;
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    if (WAIsRawNumberOrEmpty(e[@"name"])) e[@"name"] = WADisplayNameForChat(jid, e[@"name"]);
    if ([msg[@"pushName"] isKindOfClass:[NSString class]] && [msg[@"pushName"] length]) {
        [self setPushName:msg[@"pushName"] forJID:jid];
    }
    NSMutableDictionary *m = [msg mutableCopy];
    [m removeObjectForKey:@"from"];
    [self mergeMessages:@[ m ] into:e];
    unsigned long long ts = [m[@"ts"] unsignedLongLongValue];
    if (ts > [e[@"ts"] unsignedLongLongValue]) e[@"ts"] = @(ts);
    NSUInteger unread = [e[@"unread"] unsignedIntegerValue];
    e[@"unread"] = @(unread + 1);
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:jid] }];
}

- (void)markRead:(NSString *)jid
{
    if (!jid.length) return;
    NSMutableDictionary *e = [self entryForJID:jid create:NO];
    if (e && [e[@"unread"] unsignedIntegerValue] > 0) {
        e[@"unread"] = @(0);
        [self save];
        [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:jid] }];
    }
}

- (void)onMessageStatus:(NSNotification *)n
{
    NSString *mid = n.userInfo[@"id"];
    NSNumber *stNum = n.userInfo[@"status"];
    if (![mid isKindOfClass:[NSString class]] || !mid.length || !stNum) return;
    [self updateMessageStatus:[stNum integerValue] msgId:mid];
}

- (void)updateMessageStatus:(WAMessageStatus)status msgId:(NSString *)mid
{
    if (!mid.length) return;
    BOOL changed = NO;
    NSString *changedKey = nil;
    for (NSString *key in self.chats) {
        NSMutableDictionary *e = self.chats[key];
        NSMutableArray *msgs = e[@"msgs"];
        for (int i = (int)msgs.count - 1; i >= 0; i--) {
            id mObj = msgs[i];
            if (![mObj isKindOfClass:[NSDictionary class]]) continue;
            NSString *mId = mObj[@"id"];
            if ([mId isEqualToString:mid]) {
                NSMutableDictionary *m = [mObj isKindOfClass:[NSMutableDictionary class]] ? mObj : [mObj mutableCopy];
                int cur = [m[@"status"] intValue];
                if (status == WAMessageStatusError || status > cur) {
                    m[@"status"] = @(status);
                    msgs[i] = m;
                    changed = YES;
                    changedKey = key;
                }
                break;
            }
        }
        if (changed) break;
    }
    if (changed) {
        [self save];
        [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:changedKey ? @{ @"jid": changedKey } : nil];
    }
}

- (void)recordOutgoing:(NSString *)text toJID:(NSString *)jid
{
    [self recordOutgoing:text toJID:jid msgId:nil status:WAMessageStatusSent];
}

- (void)recordOutgoing:(NSString *)text toJID:(NSString *)jid msgId:(NSString *)mid status:(WAMessageStatus)status
{
    if (!jid.length) return;
    [self appendText:(text ?: @"") toJID:jid outgoing:YES ts:(unsigned long long)[[NSDate date] timeIntervalSince1970] msgId:mid status:status];
}

- (void)recordOutgoingMedia:(NSData *)thumbData caption:(NSString *)caption toJID:(NSString *)jid msgId:(NSString *)mid status:(WAMessageStatus)status
{
    if (!jid.length) return;
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    if (WAIsRawNumberOrEmpty(e[@"name"])) {
        e[@"name"] = WADisplayNameForChat(jid, e[@"name"]);
    }
    unsigned long long ts = (unsigned long long)[[NSDate date] timeIntervalSince1970];
    NSMutableDictionary *m = [NSMutableDictionary dictionaryWithObjectsAndKeys:
                              (caption ?: @""), @"text",
                              @YES, @"out",
                              @(ts), @"ts",
                              (mid ?: @""), @"id",
                              @(WA_MSG_IMAGE), @"kind",
                              @(status), @"status", nil];
    if (thumbData.length) m[@"thumb"] = thumbData;
    [self mergeMessages:@[ m ] into:e];
    if (ts > [e[@"ts"] unsignedLongLongValue]) e[@"ts"] = @(ts);
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:jid] }];
}

- (void)appendText:(NSString *)text toJID:(NSString *)jid outgoing:(BOOL)out ts:(unsigned long long)ts msgId:(NSString *)mid status:(WAMessageStatus)status
{
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    if (WAIsRawNumberOrEmpty(e[@"name"])) e[@"name"] = WADisplayNameForChat(jid, e[@"name"]);
    NSMutableDictionary *m = [NSMutableDictionary dictionaryWithObjectsAndKeys:
                              text, @"text",
                              @(out), @"out",
                              @(ts), @"ts",
                              (mid ?: @""), @"id",
                              @(WA_MSG_TEXT), @"kind",
                              @(status), @"status", nil];
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

- (void)deleteMessage:(NSDictionary *)msg inChat:(NSString *)jid
{
    if (!jid.length || !msg) return;
    NSString *key = [self keyFor:jid];
    NSMutableDictionary *e = self.chats[key];
    if (!e) return;
    NSMutableArray *msgs = e[@"msgs"];
    if (![msgs isKindOfClass:[NSMutableArray class]]) return;
    NSString *targetSig = msgSig(msg);
    BOOL found = NO;
    for (NSInteger i = (NSInteger)msgs.count - 1; i >= 0; i--) {
        if ([msgSig(msgs[i]) isEqualToString:targetSig]) {
            [msgs removeObjectAtIndex:i];
            found = YES;
            break;
        }
    }
    if (found) {
        unsigned long long last = [[[msgs lastObject] objectForKey:@"ts"] unsignedLongLongValue];
        e[@"ts"] = @(last);
        [self save];
        [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": key }];
    }
}

- (void)clearChat:(NSString *)jid
{
    if (!jid.length) return;
    NSString *key = [self keyFor:jid];
    NSMutableDictionary *e = self.chats[key];
    if (!e) return;
    NSMutableArray *msgs = e[@"msgs"];
    if ([msgs isKindOfClass:[NSMutableArray class]]) {
        [msgs removeAllObjects];
    }
    e[@"unread"] = @(0);
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": key }];
}

- (void)clearAllChats
{
    for (NSString *key in self.chats) {
        NSMutableDictionary *e = self.chats[key];
        NSMutableArray *msgs = e[@"msgs"];
        if ([msgs isKindOfClass:[NSMutableArray class]]) {
            [msgs removeAllObjects];
        }
        e[@"unread"] = @(0);
    }
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:nil];
}

- (void)refreshContactNames
{
    WAReloadAddressBookCache();
    BOOL changed = NO;
    for (NSString *key in self.chats) {
        NSMutableDictionary *e = self.chats[key];
        NSString *jid = e[@"jid"];
        NSString *currentName = e[@"name"];
        NSString *push = e[@"pushName"];
        NSString *resolved = nil;

        NSString *user = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
        NSString *ab = WAAddressBookNameForNumber(user);
        if (ab.length) {
            resolved = ab;
        } else if (currentName.length && !WAIsRawNumberOrEmpty(currentName)) {
            resolved = currentName;
        } else if (push.length && !WAIsRawNumberOrEmpty(push)) {
            resolved = push;
        } else {
            resolved = WADisplayNameForChat(jid, currentName);
        }
        if (resolved.length && ![resolved isEqualToString:currentName]) {
            e[@"name"] = resolved;
            changed = YES;
        }
    }
    if (changed) {
        [self save];
        [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:nil];
    }
}

- (void)setPushName:(NSString *)pushName forJID:(NSString *)jid
{
    if (!pushName.length || !jid.length) return;
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    e[@"pushName"] = pushName;
    if (WAIsRawNumberOrEmpty(e[@"name"])) {
        e[@"name"] = pushName;
        [self save];
        [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:jid] }];
    }
}

- (NSArray *)conversationsSorted
{
    NSMutableArray *rows = [NSMutableArray arrayWithCapacity:self.chats.count];
    for (NSString *k in self.chats) {
        NSDictionary *e = self.chats[k];
        NSDictionary *lastMsg = [e[@"msgs"] lastObject];
        NSString *last = lastMsg ? [WAChatStore previewForMessage:lastMsg] : @"";
        NSString *name = e[@"name"];
        if (WAIsRawNumberOrEmpty(name)) {
            if ([e[@"pushName"] length] && !WAIsRawNumberOrEmpty(e[@"pushName"])) {
                name = e[@"pushName"];
            } else {
                name = WADisplayNameForChat(e[@"jid"], name);
            }
        }
        [rows addObject:@{ @"jid": e[@"jid"] ?: @"",
                           @"name": name ?: @"",
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
