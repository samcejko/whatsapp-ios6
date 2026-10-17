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

        // Clean up legacy placeholder names so they can be properly resolved
        NSString *curName = entry[@"name"];
        if ([curName isEqualToString:@"WhatsApp kontakt"] || [curName isEqualToString:@"WhatsApp contact"]) {
            [entry removeObjectForKey:@"name"];
        }

        // Auto-resolve verified Meta service accounts and known contacts from linked iPhone
        NSString *jid = entry[@"jid"] ?: @"";
        if ([k isEqualToString:@"447974905007"] || [jid hasPrefix:@"447974905007"]) {
            entry[@"name"] = @"Facebook";
            entry[@"customName"] = @"Facebook";
        } else if ([k isEqualToString:@"447723442982"] || [jid hasPrefix:@"447723442982"]) {
            entry[@"name"] = @"Instagram";
            entry[@"customName"] = @"Instagram";
        } else if ([k isEqualToString:@"0"] || [jid hasPrefix:@"0@"]) {
            entry[@"name"] = @"WhatsApp";
            entry[@"customName"] = @"WhatsApp";
        } else if ([k isEqualToString:@"90297560260800"] || [jid hasPrefix:@"90297560260800"]) {
            entry[@"name"] = @"mamca💙";
            entry[@"customName"] = @"mamca💙";
            entry[@"pnJid"] = @"420608632693@s.whatsapp.net";
        } else if ([k isEqualToString:@"420608632693"] || [jid hasPrefix:@"420608632693"]) {
            entry[@"name"] = @"mamca💙";
            entry[@"customName"] = @"mamca💙";
        } else if ([k isEqualToString:@"94227857969305"] || [jid hasPrefix:@"94227857969305"]) {
            entry[@"name"] = @"domlacenej pan";
            entry[@"customName"] = @"domlacenej pan";
        }

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
    NSArray *pushnames = n.userInfo[@"pushnames"];
    if ([pushnames isKindOfClass:[NSArray class]]) {
        for (NSDictionary *pn in pushnames) {
            NSString *pid = pn[@"id"];
            NSString *pname = pn[@"name"];
            if ([pid isKindOfClass:[NSString class]] && [pname isKindOfClass:[NSString class]] && pname.length) {
                [self setPushName:pname forJID:pid];
            }
        }
    }

    NSArray *convs = n.userInfo[@"conversations"];
    if (![convs isKindOfClass:[NSArray class]]) return;
    for (NSDictionary *c in convs) {
        NSString *jid = c[@"jid"];
        if (![jid isKindOfClass:[NSString class]] || !jid.length) continue;
        NSMutableDictionary *e = [self entryForJID:jid create:YES];

        NSString *pnJid = c[@"pnJid"];
        if ([pnJid isKindOfClass:[NSString class]] && pnJid.length) {
            e[@"pnJid"] = pnJid;
        }

        NSString *name = c[@"name"];
        if ([name isKindOfClass:[NSString class]] && name.length && !WAIsRawNumberOrEmpty(name)) {
            e[@"name"] = name;
        } else if (![e[@"name"] length] || WAIsRawNumberOrEmpty(e[@"name"])) {
            if ([e[@"customName"] length]) {
                e[@"name"] = e[@"customName"];
            } else if ([e[@"pushName"] length] && !WAIsRawNumberOrEmpty(e[@"pushName"])) {
                e[@"name"] = e[@"pushName"];
            } else if ([e[@"pnJid"] length]) {
                NSString *ab = WAAddressBookNameForNumber(e[@"pnJid"]);
                if (ab.length) e[@"name"] = ab;
                else e[@"name"] = WADisplayNameForChat(e[@"pnJid"], nil);
            } else {
                e[@"name"] = WADisplayNameForChat(jid, name);
            }
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
    [self recordOutgoing:text toJID:jid msgId:mid status:status quoted:nil];
}

- (void)recordOutgoing:(NSString *)text toJID:(NSString *)jid msgId:(NSString *)mid status:(WAMessageStatus)status quoted:(NSDictionary *)quoted
{
    if (!jid.length) return;
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    if (WAIsRawNumberOrEmpty(e[@"name"])) e[@"name"] = WADisplayNameForChat(jid, e[@"name"]);
    unsigned long long ts = (unsigned long long)[[NSDate date] timeIntervalSince1970];
    NSMutableDictionary *m = [NSMutableDictionary dictionaryWithObjectsAndKeys:
                              (text ?: @""), @"text",
                              @YES, @"out",
                              @(ts), @"ts",
                              (mid ?: @""), @"id",
                              @(WA_MSG_TEXT), @"kind",
                              @(status), @"status", nil];
    if (quoted) m[@"quoted"] = quoted;
    [self mergeMessages:@[ m ] into:e];
    if (ts > [e[@"ts"] unsignedLongLongValue]) e[@"ts"] = @(ts);
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:jid] }];
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

- (void)recordOutgoingAudioDuration:(uint32_t)dur toJID:(NSString *)jid msgId:(NSString *)mid status:(WAMessageStatus)status
{
    if (!jid.length) return;
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    if (WAIsRawNumberOrEmpty(e[@"name"])) {
        e[@"name"] = WADisplayNameForChat(jid, e[@"name"]);
    }
    unsigned long long ts = (unsigned long long)[[NSDate date] timeIntervalSince1970];
    NSMutableDictionary *m = [NSMutableDictionary dictionaryWithObjectsAndKeys:
                              @"", @"text",
                              @YES, @"out",
                              @(ts), @"ts",
                              (mid ?: @""), @"id",
                              @(WA_MSG_AUDIO), @"kind",
                              @(dur), @"duration",
                              @(status), @"status", nil];
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
        case WA_MSG_IMAGE:    return WALoc(@"media_photo");
        case WA_MSG_VIDEO:    return WALoc(@"media_video");
        case WA_MSG_AUDIO:    return WALoc(@"media_voice");
        case WA_MSG_DOCUMENT: return WALoc(@"media_document");
        case WA_MSG_STICKER:  return WALoc(@"media_sticker");
        case WA_MSG_LOCATION: return WALoc(@"media_location");
        case WA_MSG_CONTACT:  return WALoc(@"media_contact");
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

- (void)toggleStarMessage:(NSDictionary *)msg inChat:(NSString *)jid
{
    if (!jid.length || !msg) return;
    NSString *key = [self keyFor:jid];
    NSMutableDictionary *e = self.chats[key];
    if (!e) return;
    NSMutableArray *msgs = e[@"msgs"];
    if (![msgs isKindOfClass:[NSMutableArray class]]) return;
    NSString *targetSig = msgSig(msg);
    BOOL changed = NO;
    for (NSInteger i = 0; i < msgs.count; i++) {
        id mObj = msgs[i];
        if ([msgSig(mObj) isEqualToString:targetSig]) {
            NSMutableDictionary *mut = [mObj isKindOfClass:[NSMutableDictionary class]] ? mObj : [mObj mutableCopy];
            BOOL cur = [mut[@"starred"] boolValue];
            mut[@"starred"] = @(!cur);
            msgs[i] = mut;
            changed = YES;
            break;
        }
    }
    if (changed) {
        [self save];
        [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": key }];
    }
}

- (BOOL)isMessageStarred:(NSDictionary *)msg inChat:(NSString *)jid
{
    if (!jid.length || !msg) return NO;
    NSString *key = [self keyFor:jid];
    NSMutableDictionary *e = self.chats[key];
    if (!e) return NO;
    NSMutableArray *msgs = e[@"msgs"];
    if (![msgs isKindOfClass:[NSMutableArray class]]) return NO;
    NSString *targetSig = msgSig(msg);
    for (NSDictionary *m in msgs) {
        if ([msgSig(m) isEqualToString:targetSig]) {
            return [m[@"starred"] boolValue];
        }
    }
    return NO;
}

- (NSArray *)starredMessagesForJID:(NSString *)jid
{
    if (!jid.length) return @[];
    NSString *key = [self keyFor:jid];
    NSMutableDictionary *e = self.chats[key];
    if (!e) return @[];
    NSMutableArray *msgs = e[@"msgs"];
    if (![msgs isKindOfClass:[NSMutableArray class]]) return @[];
    NSMutableArray *res = [NSMutableArray array];
    for (NSDictionary *m in msgs) {
        if ([m[@"starred"] boolValue]) {
            [res addObject:m];
        }
    }
    return res;
}

- (void)refreshContactNames
{
    WAReloadAddressBookCache();
    BOOL changed = NO;
    for (NSString *key in self.chats) {
        if ([key hasPrefix:@"_"]) continue;
        NSMutableDictionary *e = self.chats[key];
        NSString *jid = e[@"jid"];
        NSString *currentName = e[@"name"];
        NSString *custom = e[@"customName"];
        NSString *push = e[@"pushName"];
        NSString *pnJid = e[@"pnJid"];
        NSString *resolved = nil;

        if (custom.length && !WAIsRawNumberOrEmpty(custom)) {
            resolved = custom;
        } else {
            NSString *user = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
            NSString *ab = WAAddressBookNameForNumber(user);
            if (ab.length) {
                resolved = ab;
            } else if (pnJid.length) {
                NSString *abPn = WAAddressBookNameForNumber(pnJid);
                if (abPn.length) resolved = abPn;
            }
            if (!resolved.length && currentName.length && !WAIsRawNumberOrEmpty(currentName)) {
                resolved = currentName;
            } else if (!resolved.length && push.length && !WAIsRawNumberOrEmpty(push)) {
                resolved = push;
            } else if (!resolved.length) {
                if (pnJid.length) resolved = WADisplayNameForChat(pnJid, currentName);
                else resolved = WADisplayNameForChat(jid, currentName);
            }
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

- (void)setCustomName:(NSString *)customName forJID:(NSString *)jid
{
    if (!jid.length) return;
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    NSString *trimmed = [customName stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (trimmed.length) {
        e[@"customName"] = trimmed;
        e[@"name"] = trimmed;
    } else {
        [e removeObjectForKey:@"customName"];
        e[@"name"] = WADisplayNameForChat(jid, nil);
    }
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:jid] }];
}

- (NSArray *)conversationsSorted
{
    NSMutableArray *rows = [NSMutableArray arrayWithCapacity:self.chats.count];
    for (NSString *k in self.chats) {
        if ([k hasPrefix:@"_"]) continue;
        NSDictionary *e = self.chats[k];
        NSDictionary *lastMsg = [e[@"msgs"] lastObject];
        NSString *last = lastMsg ? [WAChatStore previewForMessage:lastMsg] : @"";
        NSString *name = e[@"customName"];
        if (WAIsRawNumberOrEmpty(name)) {
            name = e[@"name"];
        }
        if (WAIsRawNumberOrEmpty(name)) {
            if ([e[@"pushName"] length] && !WAIsRawNumberOrEmpty(e[@"pushName"])) {
                name = e[@"pushName"];
            } else if ([e[@"pnJid"] length]) {
                NSString *ab = WAAddressBookNameForNumber(e[@"pnJid"]);
                if (ab.length) name = ab;
                else name = WADisplayNameForChat(e[@"pnJid"], nil);
            } else {
                name = WADisplayNameForChat(e[@"jid"], name);
            }
        }
        BOOL isPinned = [e[@"pinned"] boolValue];
        BOOL isMuted = [self isChatMuted:e[@"jid"]];
        [rows addObject:@{ @"jid": e[@"jid"] ?: @"",
                           @"name": name ?: @"",
                           @"last": last,
                           @"ts": e[@"ts"] ?: @0,
                           @"unread": e[@"unread"] ?: @0,
                           @"pinned": @(isPinned),
                           @"muted": @(isMuted) }];
    }
    [rows sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        BOOL pa = [a[@"pinned"] boolValue], pb = [b[@"pinned"] boolValue];
        if (pa != pb) return pa ? NSOrderedAscending : NSOrderedDescending;
        unsigned long long ta = [a[@"ts"] unsignedLongLongValue], tb = [b[@"ts"] unsignedLongLongValue];
        if (ta == tb) return NSOrderedSame;
        return ta > tb ? NSOrderedAscending : NSOrderedDescending;   // newest-first
    }];
    return rows;
}

- (BOOL)isChatPinned:(NSString *)jid
{
    if (!jid.length) return NO;
    NSDictionary *e = self.chats[[self keyFor:jid]];
    return [e[@"pinned"] boolValue];
}

- (void)setChat:(NSString *)jid pinned:(BOOL)pinned
{
    if (!jid.length) return;
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    e[@"pinned"] = @(pinned);
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:nil];
}

- (BOOL)isChatMuted:(NSString *)jid
{
    if (!jid.length) return NO;
    NSDictionary *e = self.chats[[self keyFor:jid]];
    NSNumber *exp = e[@"mutedUntil"];
    if (!exp) return NO;
    NSTimeInterval val = [exp doubleValue];
    if (val < 0) return YES; // -1 = forever
    return (val > [[NSDate date] timeIntervalSince1970]);
}

- (void)muteChat:(NSString *)jid duration:(NSTimeInterval)duration
{
    if (!jid.length) return;
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    if (duration <= 0) {
        [e removeObjectForKey:@"mutedUntil"];
    } else if (duration > 1000000000) { // forever
        e[@"mutedUntil"] = @(-1);
    } else {
        NSTimeInterval expiry = [[NSDate date] timeIntervalSince1970] + duration;
        e[@"mutedUntil"] = @(expiry);
    }
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:jid] }];
}

- (NSTimeInterval)muteExpiryForJID:(NSString *)jid
{
    if (!jid.length) return 0;
    NSDictionary *e = self.chats[[self keyFor:jid]];
    return [e[@"mutedUntil"] doubleValue];
}

- (void)addReaction:(NSString *)emoji toMessageId:(NSString *)mid inChat:(NSString *)jid
{
    if (!mid.length || !jid.length) return;
    NSString *key = [self keyFor:jid];
    NSMutableDictionary *e = self.chats[key];
    if (!e) return;
    NSMutableArray *msgs = e[@"msgs"];
    BOOL changed = NO;
    for (NSInteger i = (NSInteger)msgs.count - 1; i >= 0; i--) {
        NSMutableDictionary *m = [msgs[i] isKindOfClass:[NSMutableDictionary class]] ? msgs[i] : [msgs[i] mutableCopy];
        if ([m[@"id"] isEqualToString:mid]) {
            if (emoji.length) m[@"reaction"] = emoji;
            else [m removeObjectForKey:@"reaction"];
            msgs[i] = m;
            changed = YES;
            break;
        }
    }
    if (changed) {
        [self save];
        [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": key }];
    }
}

- (NSTimeInterval)ephemeralDurationForChat:(NSString *)jid
{
    if (!jid.length) return 0;
    NSDictionary *e = self.chats[[self keyFor:jid]];
    return [e[@"ephemeralDuration"] doubleValue];
}

- (void)setEphemeralDuration:(NSTimeInterval)duration forChat:(NSString *)jid
{
    if (!jid.length) return;
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    if (duration <= 0) [e removeObjectForKey:@"ephemeralDuration"];
    else e[@"ephemeralDuration"] = @(duration);
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:jid] }];
}

- (BOOL)isContactBlocked:(NSString *)jid
{
    if (!jid.length) return NO;
    NSDictionary *e = self.chats[[self keyFor:jid]];
    return [e[@"blocked"] boolValue];
}

- (void)setContact:(NSString *)jid blocked:(BOOL)blocked
{
    if (!jid.length) return;
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    e[@"blocked"] = @(blocked);
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:jid] }];
}

- (NSArray *)blockedContacts
{
    NSMutableArray *res = [NSMutableArray array];
    for (NSString *k in self.chats) {
        if ([k hasPrefix:@"_"]) continue;
        NSDictionary *e = self.chats[k];
        if ([e isKindOfClass:[NSDictionary class]] && [e[@"blocked"] boolValue]) {
            NSString *jid = e[@"jid"];
            if (jid.length) [res addObject:jid];
        }
    }
    return res;
}

- (NSArray *)participantsForGroup:(NSString *)jid
{
    if (!jid.length) return @[];
    NSDictionary *e = self.chats[[self keyFor:jid]];
    NSArray *p = e[@"participants"];
    if ([p isKindOfClass:[NSArray class]] && p.count > 0) return p;

    NSMutableArray *def = [NSMutableArray array];
    NSString *own = [WASession shared].ownJID;
    if (own.length) [def addObject:own];

    for (NSString *k in self.chats) {
        if ([k hasPrefix:@"_"]) continue;
        NSDictionary *c = self.chats[k];
        NSString *cjid = c[@"jid"];
        if (cjid.length && [cjid rangeOfString:@"@g.us"].location == NSNotFound && ![cjid isEqualToString:own]) {
            [def addObject:cjid];
            if (def.count >= 6) break;
        }
    }
    NSMutableDictionary *me = [self entryForJID:jid create:YES];
    me[@"participants"] = def;
    [self save];
    return def;
}

- (void)addParticipant:(NSString *)participantJID toGroup:(NSString *)groupJID
{
    if (!participantJID.length || !groupJID.length) return;
    NSMutableDictionary *e = [self entryForJID:groupJID create:YES];
    NSMutableArray *p = [NSMutableArray arrayWithArray:[self participantsForGroup:groupJID]];
    if (![p containsObject:participantJID]) {
        [p addObject:participantJID];
        e[@"participants"] = p;
        [self save];
        [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:groupJID] }];
    }
}

- (void)removeParticipant:(NSString *)participantJID fromGroup:(NSString *)groupJID
{
    if (!participantJID.length || !groupJID.length) return;
    NSMutableDictionary *e = [self entryForJID:groupJID create:YES];
    NSMutableArray *p = [NSMutableArray arrayWithArray:[self participantsForGroup:groupJID]];
    if ([p containsObject:participantJID]) {
        [p removeObject:participantJID];
        e[@"participants"] = p;
        [self save];
        [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:groupJID] }];
    }
}

- (void)createBroadcastListWithRecipients:(NSArray *)recipients name:(NSString *)name
{
    if (!recipients.count) return;
    NSString *bjid = [NSString stringWithFormat:@"%u@broadcast", (unsigned int)arc4random()];
    NSMutableDictionary *e = [self entryForJID:bjid create:YES];
    e[@"jid"] = bjid;
    e[@"name"] = name.length ? name : [NSString stringWithFormat:@"%@ (%lu)", WALoc(@"broadcast_title"), (unsigned long)recipients.count];
    e[@"recipients"] = [NSMutableArray arrayWithArray:recipients];
    e[@"ts"] = @((int)[[NSDate date] timeIntervalSince1970]);
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:nil];
}

- (NSArray *)recipientsForBroadcast:(NSString *)jid
{
    if (!jid.length) return @[];
    NSDictionary *e = self.chats[[self keyFor:jid]];
    NSArray *r = e[@"recipients"];
    return [r isKindOfClass:[NSArray class]] ? r : @[];
}

- (NSDictionary *)myStatus
{
    NSDictionary *s = self.chats[@"_my_status"];
    return [s isKindOfClass:[NSDictionary class]] ? s : nil;
}

- (UIImage *)myStatusImage
{
    NSDictionary *s = [self myStatus];
    if ([s[@"hasImage"] boolValue]) {
        NSString *p = [WADocumentsPath() stringByAppendingPathComponent:@"wa6_my_status.jpg"];
        return [UIImage imageWithContentsOfFile:p];
    }
    return nil;
}

- (void)setMyStatusText:(NSString *)text
{
    [self setMyStatusText:text image:nil];
}

- (void)setMyStatusText:(NSString *)text image:(UIImage *)image
{
    if (!text.length && !image) return;
    BOOL hasImg = NO;
    if (image) {
        NSData *d = UIImageJPEGRepresentation(image, 0.85);
        if (d.length) {
            NSString *p = [WADocumentsPath() stringByAppendingPathComponent:@"wa6_my_status.jpg"];
            [d writeToFile:p atomically:YES];
            hasImg = YES;
        }
    }
    self.chats[@"_my_status"] = @{
        @"text": text ?: @"",
        @"hasImage": @(hasImg),
        @"time": @"Před chvílí",
        @"ts": @((int)[[NSDate date] timeIntervalSince1970])
    };
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:nil];
}

- (NSArray *)contactStatuses
{
    NSMutableArray *res = [NSMutableArray array];
    for (NSDictionary *c in [self conversationsSorted]) {
        NSString *jid = c[@"jid"];
        if (!jid.length || [jid hasSuffix:@"@g.us"] || [jid hasSuffix:@"@broadcast"]) continue;
        NSString *name = c[@"name"];
        NSString *last = c[@"last"];
        NSMutableDictionary *entry = self.chats[[self keyFor:jid]];
        NSString *stText = entry[@"statusText"];
        if (!stText.length) {
            stText = last.length ? last : @"Hey there! I am using WhatsApp.";
        }
        [res addObject:@{
            @"jid": jid,
            @"name": WADisplayNameForChat(jid, name),
            @"text": stText,
            @"ts": entry[@"statusTs"] ?: (c[@"ts"] ?: @((int)[[NSDate date] timeIntervalSince1970] - 3600)),
            @"time": entry[@"statusTime"] ?: WAListTime([c[@"ts"] unsignedLongLongValue])
        }];
        if (res.count >= 15) break;
    }
    return res;
}

- (void)recordContactStatus:(NSString *)text forJID:(NSString *)jid
{
    if (!text.length || !jid.length) return;
    NSMutableDictionary *e = [self entryForJID:jid create:YES];
    e[@"statusText"] = text;
    e[@"statusTime"] = @"Před chvílí";
    e[@"statusTs"] = @((int)[[NSDate date] timeIntervalSince1970]);
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:nil];
}

- (void)recordCallLog:(NSDictionary *)log
{
    if (!log) return;
    NSMutableArray *logs = self.chats[@"_call_logs"];
    if (![logs isKindOfClass:[NSMutableArray class]]) {
        logs = [NSMutableArray array];
        self.chats[@"_call_logs"] = logs;
    }
    [logs insertObject:log atIndex:0];
    if (logs.count > 100) [logs removeLastObject];
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:nil];
}

- (NSArray *)callLogs
{
    NSArray *logs = self.chats[@"_call_logs"];
    return [logs isKindOfClass:[NSArray class]] ? logs : @[];
}

- (void)clearCallLogs
{
    [self.chats removeObjectForKey:@"_call_logs"];
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:nil];
}


- (NSArray *)messagesForJID:(NSString *)jid
{
    NSDictionary *e = self.chats[[self keyFor:jid]];
    NSArray *msgs = e[@"msgs"];
    return [msgs isKindOfClass:[NSArray class]] ? msgs : @[];
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

@end
