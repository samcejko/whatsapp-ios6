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

static BOOL g_storeInitializing = NO;

- (instancetype)init
{
    if ((self = [super init])) {
        g_storeInitializing = YES;
        [self load];
        g_storeInitializing = NO;
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        [nc addObserver:self selector:@selector(onConversations:) name:WASessionConversations object:nil];
        [nc addObserver:self selector:@selector(onMessage:) name:WASessionMessage object:nil];
        [nc addObserver:self selector:@selector(onMessageStatus:) name:WASessionMessageStatus object:nil];
    }
    return self;
}

- (NSString *)path { return [WADocumentsPath() stringByAppendingPathComponent:@"wa6.chats.plist"]; }

+ (NSString *)rawDigitsForJID:(NSString *)jid
{
    if (!jid.length) return @"";
    NSString *user = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
    user = [user componentsSeparatedByString:@":"].firstObject ?: user;
    user = [user componentsSeparatedByString:@"_"].firstObject ?: user;
    NSString *digits = [[user componentsSeparatedByCharactersInSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]] componentsJoinedByString:@""];
    return digits.length ? digits : jid;
}

// Canonical key for a JID: strips :device and @server, then unifies @lid and @s.whatsapp.net keys if mapped.
+ (NSString *)keyForJID:(NSString *)jid
{
    if (!jid.length) return @"";
    NSString *digits = [self rawDigitsForJID:jid];
    if (!g_storeInitializing) {
        WAChatStore *s = [WAChatStore shared];
        if (s && s.chats) {
            return [s keyFor:jid];
        }
    }
    return digits;
}

- (NSString *)keyFor:(NSString *)jid
{
    if (!jid.length) return @"";
    NSString *digits = [WAChatStore rawDigitsForJID:jid];
    NSDictionary *lidToPn = self.chats[@"_lid_to_pn"];
    NSDictionary *pnToLid = self.chats[@"_pn_to_lid"];
    NSString *alt = [lidToPn isKindOfClass:[NSDictionary class]] ? lidToPn[digits] : nil;
    NSString *lidKey = nil;
    NSString *pnKey = nil;
    if (alt.length) {
        lidKey = digits;
        pnKey = alt;
    } else {
        alt = [pnToLid isKindOfClass:[NSDictionary class]] ? pnToLid[digits] : nil;
        if (alt.length) {
            lidKey = alt;
            pnKey = digits;
        }
    }
    if (lidKey.length && pnKey.length) {
        if (self.chats[lidKey] && self.chats[pnKey]) {
            [self mergeDuplicateChatFromKey:pnKey intoKey:lidKey];
        }
        if (self.chats[lidKey]) return lidKey;
        if (self.chats[pnKey]) return pnKey;
        return lidKey;
    }
    return digits;
}

- (void)mergeDuplicateChatFromKey:(NSString *)srcKey intoKey:(NSString *)dstKey
{
    if (!srcKey.length || !dstKey.length || [srcKey isEqualToString:dstKey]) return;
    NSMutableDictionary *src = self.chats[srcKey];
    NSMutableDictionary *dst = self.chats[dstKey];
    if (![src isKindOfClass:[NSMutableDictionary class]] || ![dst isKindOfClass:[NSMutableDictionary class]]) return;

    if ([src[@"msgs"] isKindOfClass:[NSArray class]] && [src[@"msgs"] count] > 0) {
        [self mergeMessages:src[@"msgs"] into:dst];
    }
    if ([src[@"ts"] unsignedLongLongValue] > [dst[@"ts"] unsignedLongLongValue]) {
        dst[@"ts"] = src[@"ts"];
    }
    if ([src[@"pinned"] boolValue]) dst[@"pinned"] = @YES;
    if ([src[@"customName"] length] && ![dst[@"customName"] length]) dst[@"customName"] = src[@"customName"];
    if ([src[@"pushName"] length] && ![dst[@"pushName"] length]) dst[@"pushName"] = src[@"pushName"];
    if ([src[@"pnJid"] length] && ![dst[@"pnJid"] length]) dst[@"pnJid"] = src[@"pnJid"];
    if ([src[@"lidJid"] length] && ![dst[@"lidJid"] length]) dst[@"lidJid"] = src[@"lidJid"];
    if ([src[@"jid"] hasSuffix:@"@s.whatsapp.net"] && ![dst[@"pnJid"] length]) dst[@"pnJid"] = src[@"jid"];
    if ([src[@"jid"] hasSuffix:@"@lid"] && ![dst[@"lidJid"] length]) dst[@"lidJid"] = src[@"jid"];
    if (!WAIsRawNumberOrEmpty(src[@"name"]) && WAIsRawNumberOrEmpty(dst[@"name"])) {
        dst[@"name"] = src[@"name"];
    }
    [self.chats removeObjectForKey:srcKey];
}

- (void)registerLidMapping:(NSString *)lidJid pnJid:(NSString *)pnJid
{
    if (!lidJid.length || !pnJid.length) return;
    NSString *lidKey = [WAChatStore rawDigitsForJID:lidJid];
    NSString *pnKey = [WAChatStore rawDigitsForJID:pnJid];
    if (!lidKey.length || !pnKey.length || [lidKey isEqualToString:pnKey]) return;

    NSMutableDictionary *lidToPn = self.chats[@"_lid_to_pn"];
    if (![lidToPn isKindOfClass:[NSMutableDictionary class]]) {
        lidToPn = [NSMutableDictionary dictionary];
        self.chats[@"_lid_to_pn"] = lidToPn;
    }
    NSMutableDictionary *pnToLid = self.chats[@"_pn_to_lid"];
    if (![pnToLid isKindOfClass:[NSMutableDictionary class]]) {
        pnToLid = [NSMutableDictionary dictionary];
        self.chats[@"_pn_to_lid"] = pnToLid;
    }
    lidToPn[lidKey] = pnKey;
    pnToLid[pnKey] = lidKey;

    NSString *cleanPn = [[pnJid componentsSeparatedByString:@"@"].firstObject componentsSeparatedByString:@":"].firstObject;
    if (cleanPn.length) cleanPn = [cleanPn stringByAppendingString:@"@s.whatsapp.net"];
    NSString *cleanLid = [[lidJid componentsSeparatedByString:@"@"].firstObject componentsSeparatedByString:@":"].firstObject;
    if (cleanLid.length) cleanLid = [cleanLid stringByAppendingString:@"@lid"];

    if (self.chats[lidKey] && self.chats[pnKey]) {
        // Keep the entry that uses @lid (or has more messages) and merge the other into it
        [self mergeDuplicateChatFromKey:pnKey intoKey:lidKey];
    }
    NSMutableDictionary *e = self.chats[lidKey] ?: self.chats[pnKey];
    if ([e isKindOfClass:[NSMutableDictionary class]]) {
        if (cleanPn.length) e[@"pnJid"] = cleanPn;
        if (cleanLid.length) e[@"lidJid"] = cleanLid;
    }
}

- (void)registerContactName:(NSString *)name pushName:(NSString *)pushName lidJid:(NSString *)lidJid pnJid:(NSString *)pnJid
{
    if (lidJid.length && pnJid.length) {
        [self registerLidMapping:lidJid pnJid:pnJid];
    }
    NSMutableDictionary *contacts = self.chats[@"_contacts"];
    if (![contacts isKindOfClass:[NSMutableDictionary class]]) {
        contacts = [NSMutableDictionary dictionary];
        self.chats[@"_contacts"] = contacts;
    }
    NSString *lidKey = [WAChatStore rawDigitsForJID:lidJid];
    NSString *pnKey = [WAChatStore rawDigitsForJID:pnJid];
    NSArray *keys = @[ lidKey ?: @"", pnKey ?: @"" ];
    for (NSString *k in keys) {
        if (!k.length) continue;
        NSMutableDictionary *c = [contacts[k] isKindOfClass:[NSMutableDictionary class]]
            ? contacts[k]
            : ([contacts[k] isKindOfClass:[NSDictionary class]] ? [contacts[k] mutableCopy] : [NSMutableDictionary dictionary]);
        if (name.length && !WAIsRawNumberOrEmpty(name)) c[@"name"] = name;
        if (pushName.length && !WAIsRawNumberOrEmpty(pushName)) c[@"pushName"] = pushName;
        if (lidJid.length) c[@"lidJid"] = lidJid;
        if (pnJid.length) c[@"pnJid"] = pnJid;
        contacts[k] = c;

        NSMutableDictionary *chat = self.chats[k];
        if ([chat isKindOfClass:[NSMutableDictionary class]] && ![chat[@"jid"] hasSuffix:@"@g.us"]) {
            if (name.length && !WAIsRawNumberOrEmpty(name)) {
                chat[@"name"] = name;
            } else if (pushName.length && !WAIsRawNumberOrEmpty(pushName)) {
                chat[@"pushName"] = pushName;
                if (WAIsRawNumberOrEmpty(chat[@"name"])) chat[@"name"] = pushName;
            }
            if (pnJid.length && ![chat[@"pnJid"] length]) chat[@"pnJid"] = pnJid;
            if (lidJid.length && ![chat[@"lidJid"] length]) chat[@"lidJid"] = lidJid;
        }
    }
}

- (NSString *)phoneNumberJIDForJID:(NSString *)jid
{
    if (!jid.length) return nil;
    if ([jid hasSuffix:@"@s.whatsapp.net"]) {
        NSString *u = [[jid componentsSeparatedByString:@"@"].firstObject componentsSeparatedByString:@":"].firstObject;
        return u.length ? [u stringByAppendingString:@"@s.whatsapp.net"] : jid;
    }
    NSString *digits = [WAChatStore rawDigitsForJID:jid];
    NSDictionary *e = self.chats[[self keyFor:jid]];
    if ([e isKindOfClass:[NSDictionary class]] && [e[@"pnJid"] length]) return e[@"pnJid"];
    NSDictionary *contacts = self.chats[@"_contacts"];
    if ([contacts isKindOfClass:[NSDictionary class]]) {
        NSDictionary *c = contacts[digits];
        if ([c isKindOfClass:[NSDictionary class]] && [c[@"pnJid"] length]) return c[@"pnJid"];
    }
    NSDictionary *lidToPn = self.chats[@"_lid_to_pn"];
    if ([lidToPn isKindOfClass:[NSDictionary class]]) {
        NSString *pnDigits = lidToPn[digits];
        if (pnDigits.length) return [pnDigits stringByAppendingString:@"@s.whatsapp.net"];
    }
    return nil;
}

- (NSString *)lidJIDForJID:(NSString *)jid
{
    if (!jid.length) return nil;
    if ([jid hasSuffix:@"@lid"]) {
        NSString *u = [[jid componentsSeparatedByString:@"@"].firstObject componentsSeparatedByString:@":"].firstObject;
        return u.length ? [u stringByAppendingString:@"@lid"] : jid;
    }
    NSString *digits = [WAChatStore rawDigitsForJID:jid];
    NSDictionary *e = self.chats[[self keyFor:jid]];
    if ([e isKindOfClass:[NSDictionary class]] && [e[@"lidJid"] length]) return e[@"lidJid"];
    NSDictionary *pnToLid = self.chats[@"_pn_to_lid"];
    if ([pnToLid isKindOfClass:[NSDictionary class]]) {
        NSString *lidDigits = pnToLid[digits];
        if (lidDigits.length) return [lidDigits stringByAppendingString:@"@lid"];
    }
    return nil;
}

- (NSString *)displayNameForJID:(NSString *)jid
{
    if (!jid.length) return nil;
    NSString *digits = [WAChatStore rawDigitsForJID:jid];
    NSString *key = [self keyFor:jid];
    NSDictionary *e = self.chats[key];
    if ([e isKindOfClass:[NSDictionary class]]) {
        if ([e[@"customName"] length] && !WAIsRawNumberOrEmpty(e[@"customName"])) return e[@"customName"];
        if ([e[@"name"] length] && !WAIsRawNumberOrEmpty(e[@"name"])) return e[@"name"];
    }
    NSDictionary *contacts = self.chats[@"_contacts"];
    if ([contacts isKindOfClass:[NSDictionary class]]) {
        NSDictionary *c = contacts[digits] ?: contacts[key];
        if (!c) {
            NSDictionary *lidToPn = self.chats[@"_lid_to_pn"];
            NSDictionary *pnToLid = self.chats[@"_pn_to_lid"];
            NSString *alt = [lidToPn isKindOfClass:[NSDictionary class]] ? lidToPn[digits] : nil;
            if (!alt.length && [pnToLid isKindOfClass:[NSDictionary class]]) alt = pnToLid[digits];
            if (alt.length) c = contacts[alt];
        }
        if ([c isKindOfClass:[NSDictionary class]]) {
            if ([c[@"name"] length] && !WAIsRawNumberOrEmpty(c[@"name"])) return c[@"name"];
            if ([c[@"pushName"] length] && !WAIsRawNumberOrEmpty(c[@"pushName"])) return c[@"pushName"];
        }
    }
    if ([e isKindOfClass:[NSDictionary class]] && [e[@"pushName"] length] && !WAIsRawNumberOrEmpty(e[@"pushName"])) {
        return e[@"pushName"];
    }
    return nil;
}

- (void)load
{
    self.chats = [NSMutableDictionary dictionary];
    self.chats[@"_contacts"] = [NSMutableDictionary dictionary];
    self.chats[@"_lid_to_pn"] = [NSMutableDictionary dictionary];
    self.chats[@"_pn_to_lid"] = [NSMutableDictionary dictionary];

    NSDictionary *disk = [NSDictionary dictionaryWithContentsOfFile:[self path]];
    if ([disk[@"_contacts"] isKindOfClass:[NSDictionary class]]) {
        NSMutableDictionary *mc = [NSMutableDictionary dictionary];
        for (NSString *ck in disk[@"_contacts"]) {
            if ([disk[@"_contacts"][ck] isKindOfClass:[NSDictionary class]])
                mc[ck] = [disk[@"_contacts"][ck] mutableCopy];
        }
        self.chats[@"_contacts"] = mc;
    }
    if ([disk[@"_lid_to_pn"] isKindOfClass:[NSDictionary class]]) {
        self.chats[@"_lid_to_pn"] = [disk[@"_lid_to_pn"] mutableCopy];
    }
    if ([disk[@"_pn_to_lid"] isKindOfClass:[NSDictionary class]]) {
        self.chats[@"_pn_to_lid"] = [disk[@"_pn_to_lid"] mutableCopy];
    }

    for (NSString *k in disk) {
        if ([k hasPrefix:@"_"]) {
            if (![k isEqualToString:@"_contacts"] && ![k isEqualToString:@"_lid_to_pn"] && ![k isEqualToString:@"_pn_to_lid"]) {
                id obj = disk[k];
                self.chats[k] = [obj respondsToSelector:@selector(mutableCopy)] ? [obj mutableCopy] : obj;
            }
            continue;
        }
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

        NSString *curName = entry[@"name"];
        if ([curName isEqualToString:@"WhatsApp kontakt"] || [curName isEqualToString:@"WhatsApp contact"] ||
            [curName isEqualToString:@"Skupinový chat"] || [curName isEqualToString:@"Group chat"]) {
            [entry removeObjectForKey:@"name"];
        }

        // If this is a ghost chat created by old setPushName:forJID: (0 messages, ts == 0, not pinned),
        // harvest its contact metadata into _contacts and skip adding it as a conversation row!
        unsigned long long ts = [entry[@"ts"] unsignedLongLongValue];
        if (mutMsgs.count == 0 && ts == 0 && ![entry[@"pinned"] boolValue]) {
            NSString *jid = entry[@"jid"] ?: @"";
            NSString *pn = entry[@"pnJid"];
            NSString *lid = [jid hasSuffix:@"@lid"] ? jid : entry[@"lidJid"];
            if ([jid hasSuffix:@"@s.whatsapp.net"] && !pn.length) pn = jid;
            [self registerContactName:entry[@"name"] pushName:entry[@"pushName"] lidJid:lid pnJid:pn];
            continue;
        }

        self.chats[k] = entry;
    }

    // Load optional synced contacts seed file (wa6.contacts.plist in Documents or Bundle)
    NSArray *seedPaths = @[
        [WADocumentsPath() stringByAppendingPathComponent:@"wa6.contacts.plist"],
        [[NSBundle mainBundle] pathForResource:@"wa6.contacts" ofType:@"plist"] ?: @""
    ];
    for (NSString *sp in seedPaths) {
        if (!sp.length || ![[NSFileManager defaultManager] fileExistsAtPath:sp]) continue;
        NSDictionary *seed = [NSDictionary dictionaryWithContentsOfFile:sp];
        if (![seed isKindOfClass:[NSDictionary class]]) continue;
        NSArray *cList = seed[@"contacts"];
        if ([cList isKindOfClass:[NSArray class]]) {
            for (NSDictionary *item in cList) {
                if (![item isKindOfClass:[NSDictionary class]]) continue;
                [self registerContactName:item[@"name"]
                                 pushName:item[@"pushName"]
                                   lidJid:item[@"lid"]
                                    pnJid:item[@"pn"]];
            }
        }
        NSArray *gList = seed[@"groups"];
        if ([gList isKindOfClass:[NSArray class]]) {
            for (NSDictionary *g in gList) {
                if (![g isKindOfClass:[NSDictionary class]]) continue;
                NSString *gjid = g[@"jid"];
                NSString *subj = g[@"subject"];
                NSArray *parts = g[@"participants"];
                if (gjid.length) {
                    NSMutableDictionary *ge = self.chats[[self keyFor:gjid]];
                    if ([ge isKindOfClass:[NSMutableDictionary class]]) {
                        if (subj.length) ge[@"name"] = subj;
                        if ([parts isKindOfClass:[NSArray class]] && parts.count)
                            ge[@"participants"] = [parts mutableCopy];
                    }
                }
            }
        }
        NSDictionary *msgSenders = seed[@"msgSenders"];
        if ([msgSenders isKindOfClass:[NSDictionary class]] && msgSenders.count) {
            for (NSString *ck in [self.chats allKeys]) {
                if ([ck hasPrefix:@"_"]) continue;
                NSMutableDictionary *ce = self.chats[ck];
                if (![ce isKindOfClass:[NSMutableDictionary class]]) continue;
                NSArray *cmsgs = ce[@"msgs"];
                if (![cmsgs isKindOfClass:[NSArray class]]) continue;
                for (NSMutableDictionary *cm in cmsgs) {
                    if (![cm isKindOfClass:[NSMutableDictionary class]]) continue;
                    NSString *mid = cm[@"id"];
                    if (mid.length && msgSenders[mid] && ![cm[@"out"] boolValue]) {
                        cm[@"sender"] = msgSenders[mid];
                    }
                }
            }
        }
    }

    // Ensure built-in known mappings and merge any duplicate LID/PN chats
    [self registerContactName:@"mamca💙" pushName:@"Lucie Cejkova" lidJid:@"90297560260800@lid" pnJid:@"420608632693@s.whatsapp.net"];
    [self registerContactName:@"domlacenej pan" pushName:@"Ondřej Čejka" lidJid:@"94227857969305@lid" pnJid:@"420731478099@s.whatsapp.net"];

    NSDictionary *lidToPnSnap = [self.chats[@"_lid_to_pn"] copy];
    for (NSString *lidK in lidToPnSnap) {
        NSString *pnK = lidToPnSnap[lidK];
        if (self.chats[lidK] && self.chats[pnK]) {
            [self mergeDuplicateChatFromKey:pnK intoKey:lidK];
        }
    }

    // Re-resolve names on all chat entries
    for (NSString *k in [self.chats allKeys]) {
        if ([k hasPrefix:@"_"]) continue;
        NSMutableDictionary *e = self.chats[k];
        if (![e isKindOfClass:[NSMutableDictionary class]]) continue;
        NSString *jid = e[@"jid"] ?: @"";
        NSString *resolved = [self displayNameForJID:jid];
        if (resolved.length && !WAIsRawNumberOrEmpty(resolved)) {
            e[@"name"] = resolved;
        }
    }
}

- (void)save
{
    if (![self.chats writeToFile:[self path] atomically:YES]) WALog(@"chat store: save failed");
}

- (NSMutableDictionary *)entryForJID:(NSString *)jid create:(BOOL)create
{
    NSString *key = [self keyFor:jid];
    NSMutableDictionary *e = self.chats[key];
    if (!e && create) {
        e = [NSMutableDictionary dictionary];
        e[@"jid"] = jid ?: @"";
        NSString *pn = [self phoneNumberJIDForJID:jid];
        if (pn.length) e[@"pnJid"] = pn;
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
    NSArray *mappings = n.userInfo[@"mappings"];
    if ([mappings isKindOfClass:[NSArray class]]) {
        for (NSDictionary *mp in mappings) {
            NSString *pn = mp[@"pnJid"];
            NSString *lid = mp[@"lidJid"];
            if ([pn isKindOfClass:[NSString class]] && [lid isKindOfClass:[NSString class]] && pn.length && lid.length) {
                [self registerLidMapping:lid pnJid:pn];
            }
        }
    }

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

        NSString *pnJid = c[@"pnJid"];
        NSString *lidJid = c[@"lidJid"];
        if ([jid hasSuffix:@"@lid"] && [pnJid isKindOfClass:[NSString class]] && pnJid.length) {
            [self registerLidMapping:jid pnJid:pnJid];
        } else if ([jid hasSuffix:@"@s.whatsapp.net"] && [lidJid isKindOfClass:[NSString class]] && lidJid.length) {
            [self registerLidMapping:lidJid pnJid:jid];
        }

        NSMutableDictionary *e = [self entryForJID:jid create:YES];
        if ([pnJid isKindOfClass:[NSString class]] && pnJid.length) e[@"pnJid"] = pnJid;
        if ([lidJid isKindOfClass:[NSString class]] && lidJid.length) e[@"lidJid"] = lidJid;

        NSString *name = c[@"name"];
        if ([name isKindOfClass:[NSString class]] && name.length && !WAIsRawNumberOrEmpty(name)) {
            e[@"name"] = name;
            if (![jid hasSuffix:@"@g.us"]) {
                [self registerContactName:name pushName:nil
                                   lidJid:([jid hasSuffix:@"@lid"] ? jid : lidJid)
                                    pnJid:([jid hasSuffix:@"@s.whatsapp.net"] ? jid : pnJid)];
            }
        } else {
            NSString *resolved = [self displayNameForJID:jid];
            if (resolved.length && !WAIsRawNumberOrEmpty(resolved)) {
                e[@"name"] = resolved;
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
    BOOL isGroup = ([jid rangeOfString:@"@g.us"].location != NSNotFound);
    if (!isGroup && [msg[@"pushName"] isKindOfClass:[NSString class]] && [msg[@"pushName"] length]) {
        [self setPushName:msg[@"pushName"] forJID:jid];
    }
    if (WAIsRawNumberOrEmpty(e[@"name"])) e[@"name"] = WADisplayNameForChat(jid, e[@"name"]);
    NSMutableDictionary *m = [msg mutableCopy];
    [m removeObjectForKey:@"from"];
    [self mergeMessages:@[ m ] into:e];
    unsigned long long ts = [m[@"ts"] unsignedLongLongValue];
    if (ts > [e[@"ts"] unsignedLongLongValue]) e[@"ts"] = @(ts);
    if (![m[@"out"] boolValue]) {
        NSUInteger unread = [e[@"unread"] unsignedIntegerValue];
        e[@"unread"] = @(unread + 1);
    }
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
    for (NSString *key in [self.chats allKeys]) {
        if ([key hasPrefix:@"_"]) continue;
        NSMutableDictionary *e = self.chats[key];
        if (![e isKindOfClass:[NSMutableDictionary class]]) continue;
        NSString *jid = e[@"jid"];
        NSString *currentName = e[@"name"];
        NSString *custom = e[@"customName"];
        NSString *resolved = nil;

        if (custom.length && !WAIsRawNumberOrEmpty(custom)) {
            resolved = custom;
        } else {
            NSString *user = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
            NSString *ab = WAAddressBookNameForNumber(user);
            NSString *pnJid = [self phoneNumberJIDForJID:jid];
            if (!ab.length && pnJid.length) {
                ab = WAAddressBookNameForNumber(pnJid);
            }
            if (ab.length) {
                resolved = ab;
            } else {
                resolved = [self displayNameForJID:jid];
                if (!resolved.length || WAIsRawNumberOrEmpty(resolved)) {
                    resolved = WADisplayNameForChat(pnJid.length ? pnJid : jid, currentName);
                }
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
    if (!pushName.length || !jid.length || WAIsRawNumberOrEmpty(pushName)) return;
    if ([jid rangeOfString:@"@g.us"].location != NSNotFound) return;

    NSString *lid = [jid hasSuffix:@"@lid"] ? jid : [self lidJIDForJID:jid];
    NSString *pn = [jid hasSuffix:@"@s.whatsapp.net"] ? jid : [self phoneNumberJIDForJID:jid];
    [self registerContactName:nil pushName:pushName lidJid:lid pnJid:pn];

    NSMutableDictionary *e = [self entryForJID:jid create:NO];
    if (e) {
        e[@"pushName"] = pushName;
        if (WAIsRawNumberOrEmpty(e[@"name"])) {
            e[@"name"] = pushName;
            [self save];
            [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:jid] }];
        }
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
        if (![e isKindOfClass:[NSDictionary class]]) continue;
        NSArray *msgs = e[@"msgs"];
        unsigned long long ts = [e[@"ts"] unsignedLongLongValue];
        BOOL isPinned = [e[@"pinned"] boolValue];
        if ((![msgs isKindOfClass:[NSArray class]] || msgs.count == 0) && ts == 0 && !isPinned) continue;

        NSDictionary *lastMsg = [msgs isKindOfClass:[NSArray class]] ? [msgs lastObject] : nil;
        NSString *last = lastMsg ? [WAChatStore previewForMessage:lastMsg] : @"";
        NSString *jid = e[@"jid"] ?: @"";
        NSString *name = [self displayNameForJID:jid];
        if (WAIsRawNumberOrEmpty(name)) {
            name = WADisplayNameForChat(jid, e[@"name"]);
        }
        BOOL isMuted = [self isChatMuted:jid];
        [rows addObject:@{ @"jid": jid,
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
    return @[];
}

- (void)setParticipants:(NSArray *)participants forGroup:(NSString *)groupJID subject:(NSString *)subject
{
    if (!groupJID.length) return;
    NSMutableDictionary *e = [self entryForJID:groupJID create:YES];
    if ([participants isKindOfClass:[NSArray class]]) {
        e[@"participants"] = [participants mutableCopy];
    }
    if (subject.length && !WAIsRawNumberOrEmpty(subject)) {
        e[@"name"] = subject;
    }
    [self save];
    [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [self keyFor:groupJID] }];
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
        NSMutableDictionary *entry = self.chats[[self keyFor:jid]];
        NSString *stText = entry[@"statusText"];
        if (!stText.length) continue;
        [res addObject:@{
            @"jid": jid,
            @"name": WADisplayNameForChat(jid, c[@"name"]),
            @"text": stText,
            @"ts": entry[@"statusTs"] ?: (c[@"ts"] ?: @((int)[[NSDate date] timeIntervalSince1970] - 3600)),
            @"time": entry[@"statusTime"] ?: WAListTime([entry[@"statusTs"] unsignedLongLongValue])
        }];
        if (res.count >= 25) break;
    }
    return res;
}

- (void)recordContactStatus:(NSString *)text forJID:(NSString *)jid
{
    if (!text.length || !jid.length) return;
    NSMutableDictionary *e = [self entryForJID:jid create:NO];
    if (!e) return;
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
