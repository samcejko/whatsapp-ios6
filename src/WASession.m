#import "WASession.h"
#import "WACommon.h"

#include "wa_store.h"
#include "wa_client.h"
#include "wa_sessions.h"
#include "wa_chat.h"
#include "wa_history.h"
#include "wa_callengine.h"

NSString *const WASessionStateChanged = @"WASessionStateChanged";
NSString *const WASessionQRChanged = @"WASessionQRChanged";
NSString *const WASessionPaired = @"WASessionPaired";
NSString *const WASessionNodeReceived = @"WASessionNodeReceived";
NSString *const WASessionMessage = @"WASessionMessage";
NSString *const WASessionMessageStatus = @"WASessionMessageStatus";
NSString *const WASessionConversations = @"WASessionConversations";
NSString *const WASessionPresenceChanged = @"WASessionPresenceChanged";

@interface WASession () {
@public
    wa_device _dev;
    wa_client *_client;
    wa_client_callbacks _cb;
    wa_client_config _cfg;
    wa_sess_store *_sessions;   // Signal sessions, touched only on the client thread
    NSThread *_thread;
}
@property (nonatomic) WASessionState state;
@property (nonatomic, copy) NSString *qrString;
@property (nonatomic, copy) NSString *ownJID;
@property (nonatomic) BOOL running;
@end

static NSString *storePath(void) { return [WADocumentsPath() stringByAppendingPathComponent:@"wa6.store"]; }
static NSString *sessionsPath(void) { return [WADocumentsPath() stringByAppendingPathComponent:@"wa6.sessions"]; }

// ---- C callbacks (called on the client thread; marshalled to the main thread) ----

static void cb_qr(void *ctx, const char *qr)
{
    WASession *self = (__bridge WASession *)ctx;
    NSString *s = qr ? [NSString stringWithUTF8String:qr] : nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        self.qrString = s;
        self.state = WASessionStatePairing;
        if (s) [[NSNotificationCenter defaultCenter] postNotificationName:WASessionQRChanged object:self userInfo:@{ @"qr": s }];
        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionStateChanged object:self userInfo:@{ @"state": @(WASessionStatePairing) }];
    });
}

static void cb_paired(void *ctx, const char *jid)
{
    WASession *self = (__bridge WASession *)ctx;
    NSString *j = jid ? [NSString stringWithUTF8String:jid] : @"";
    dispatch_async(dispatch_get_main_queue(), ^{
        self.ownJID = j;
        self.qrString = nil;
        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionPaired object:self userInfo:@{ @"jid": j }];
    });
}

static void cb_connected(void *ctx)
{
    WASession *self = (__bridge WASession *)ctx;
    dispatch_async(dispatch_get_main_queue(), ^{
        self.state = WASessionStateConnected;
        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionStateChanged object:self userInfo:@{ @"state": @(WASessionStateConnected) }];
    });
}

// Acknowledge a received stanza so the server stops redelivering it.
static void send_ack(WASession *self, const wa_node *node, const char *cls)
{
    const char *id = wa_node_get(node, "id");
    const char *from = wa_node_get(node, "from");
    if (!id || !from) return;
    wa_node *ack = wa_node_new("ack");
    wa_node_attr(ack, "to", from);
    wa_node_attr(ack, "id", id);
    wa_node_attr(ack, "class", cls);
    const char *participant = wa_node_get(node, "participant");
    if (participant) wa_node_attr(ack, "participant", participant);
    wa_client_send(self->_client, ack);
    wa_node_free(ack);
}

// Runs on the client thread (from process_frames).
static void cb_node(void *ctx, const wa_node *node)
{
    WASession *self = (__bridge WASession *)ctx;

    if (wa_node_tag_is(node, "message")) {
        char from[80] = { 0 };
        wa_history_result hist;
        int is_history = 0;
        char *text = wa_chat_handle_message(self->_client, self->_sessions, node, from, sizeof from, &hist, &is_history);
        wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
        send_ack(self, node, "message");
        if (is_history && hist.nconvs > 0) {
            NSMutableArray *convs = [NSMutableArray arrayWithCapacity:hist.nconvs];
            for (int i = 0; i < hist.nconvs; i++) {
                wa_history_conv *cv = &hist.convs[i];
                NSString *jid = cv->jid ? [NSString stringWithUTF8String:cv->jid] : nil;
                if (!jid) continue;   // stringWithUTF8String returns nil on invalid UTF-8; skip rather than crash
                NSString *name = cv->name ? [NSString stringWithUTF8String:cv->name] : nil;
                NSString *last = cv->last ? [NSString stringWithUTF8String:cv->last] : nil;
                NSMutableArray *msgs = [NSMutableArray arrayWithCapacity:cv->nmsgs];
                for (int j = 0; j < cv->nmsgs; j++) {
                    wa_history_msg *mm = &cv->msgs[j];
                    NSString *mt = mm->text ? ([NSString stringWithUTF8String:mm->text] ?: @"") : @"";
                    NSMutableDictionary *md = [NSMutableDictionary dictionaryWithObjectsAndKeys:
                                               mt, @"text",
                                               @(mm->from_me != 0), @"out",
                                               @(mm->timestamp), @"ts",
                                               @(mm->kind), @"kind", nil];
                    int st = mm->status;
                    if (mm->from_me && st == 0) st = WA_STATUS_DELIVERY_ACK;
                    md[@"status"] = @(st);
                    NSString *mid = mm->id ? [NSString stringWithUTF8String:mm->id] : nil;
                    if (mid) md[@"id"] = mid;
                    if (mm->duration) md[@"dur"] = @(mm->duration);
                    if (mm->filename) { NSString *fn = [NSString stringWithUTF8String:mm->filename]; if (fn) md[@"file"] = fn; }
                    if (mm->thumb && mm->thumb_len) md[@"thumb"] = [NSData dataWithBytes:mm->thumb length:mm->thumb_len];
                    if (mm->sender) { NSString *sn = [NSString stringWithUTF8String:mm->sender]; if (sn.length) md[@"sender"] = sn; }
                    if (mm->direct_path) { NSString *dp = [NSString stringWithUTF8String:mm->direct_path]; if (dp.length) md[@"dpath"] = dp; }
                    if (mm->media_key && mm->media_key_len) md[@"mkey"] = [NSData dataWithBytes:mm->media_key length:mm->media_key_len];
                    if (mm->enc_sha && mm->enc_sha_len) md[@"esha"] = [NSData dataWithBytes:mm->enc_sha length:mm->enc_sha_len];
                    [msgs addObject:md];
                }
                [convs addObject:@{ @"jid": jid,
                                    @"name": name ?: @"",
                                    @"last": last ?: @"",
                                    @"ts": @(cv->timestamp),
                                    @"unread": @(cv->unread),
                                    @"messages": msgs }];
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionConversations object:self userInfo:@{ @"conversations": convs }];
            });
        }
        wa_history_result_free(&hist);
        if (text) {
            NSString *f = [NSString stringWithUTF8String:from];
            NSString *t = [NSString stringWithUTF8String:text];
            free(text);
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessage object:self userInfo:@{ @"from": f ?: @"", @"text": t ?: @"" }];
            });
        }
        return;
    }
    if (wa_node_tag_is(node, "call")) {
        send_ack(self, node, "call");
        wa_call_state st;
        if (wa_callengine_on_offer(self->_client, self->_sessions, node, &st)) {
            wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
            // (an incoming-call UI could be raised here; the offer's call key is now decrypted)
            WALog(@"incoming call from %s (video=%d, key %s)", st.peer, st.video, st.active ? "ok" : "missing");
        }
        return;
    }
    if (wa_node_tag_is(node, "ack")) {
        const char *cls = wa_node_get(node, "class");
        if (cls && strcmp(cls, "message") == 0) {
            const char *aid = wa_node_get(node, "id");
            const char *err = wa_node_get(node, "error");
            if (aid && aid[0]) {
                NSString *mid = [NSString stringWithUTF8String:aid];
                int st = err ? WA_STATUS_ERROR : WA_STATUS_SERVER_ACK;
                NSString *errStr = err ? [NSString stringWithUTF8String:err] : nil;
                if (mid) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        NSMutableDictionary *ui = [NSMutableDictionary dictionaryWithObjectsAndKeys:mid, @"id", @(st), @"status", nil];
                        if (errStr) ui[@"error"] = errStr;
                        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus object:self userInfo:ui];
                    });
                }
            }
        }
    }
    if (wa_node_tag_is(node, "receipt")) {
        send_ack(self, node, "receipt");
        const char *rtype = wa_node_get(node, "type");
        int st = WA_STATUS_DELIVERY_ACK;
        if (rtype) {
            if (strcmp(rtype, "read") == 0 || strcmp(rtype, "read-self") == 0) st = WA_STATUS_READ;
            else if (strcmp(rtype, "played") == 0 || strcmp(rtype, "played-self") == 0) st = WA_STATUS_PLAYED;
            else if (strcmp(rtype, "hist_sync") == 0) st = -1;
        }
        if (st >= 0) {
            const char *rid = wa_node_get(node, "id");
            if (rid && rid[0]) {
                NSString *mid = [NSString stringWithUTF8String:rid];
                if (mid) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus object:self userInfo:@{ @"id": mid, @"status": @(st) }];
                    });
                }
            }
            for (int i = 0; i < node->nchildren; i++) {
                const wa_node *child = node->children[i];
                if (wa_node_tag_is(child, "list")) {
                    for (int j = 0; j < child->nchildren; j++) {
                        const wa_node *item = child->children[j];
                        const char *iid = wa_node_get(item, "id");
                        if (iid && iid[0]) {
                            NSString *mid = [NSString stringWithUTF8String:iid];
                            if (mid) {
                                dispatch_async(dispatch_get_main_queue(), ^{
                                    [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus object:self userInfo:@{ @"id": mid, @"status": @(st) }];
                                });
                            }
                        }
                    }
                }
            }
        }
    }
    if (wa_node_tag_is(node, "presence")) {
        const char *from = wa_node_get(node, "from");
        const char *type = wa_node_get(node, "type");
        const char *last = wa_node_get(node, "last");
        if (from && from[0]) {
            NSString *f = [NSString stringWithUTF8String:from];
            NSString *t = type ? [NSString stringWithUTF8String:type] : @"available";
            NSMutableDictionary *ui = [NSMutableDictionary dictionaryWithObjectsAndKeys:f, @"jid", t, @"state", nil];
            if (last) ui[@"last"] = [NSString stringWithUTF8String:last];
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionPresenceChanged object:self userInfo:ui];
            });
        }
    }
    if (wa_node_tag_is(node, "chatstate")) {
        const char *from = wa_node_get(node, "from");
        if (from && from[0]) {
            NSString *f = [NSString stringWithUTF8String:from];
            NSString *state = @"paused";
            if (wa_node_child(node, "composing")) state = @"composing";
            else if (wa_node_child(node, "paused")) state = @"paused";
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionPresenceChanged object:self userInfo:@{ @"jid": f, @"state": state }];
            });
        }
    }

    char *s = wa_node_to_string(node, 0);
    NSString *xml = s ? [NSString stringWithUTF8String:s] : nil;
    free(s);
    if (!xml) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionNodeReceived object:self userInfo:@{ @"xml": xml }];
    });
}

static void cb_disconnect(void *ctx, const char *reason, int reconnect)
{
    WASession *self = (__bridge WASession *)ctx;
    NSString *r = reason ? [NSString stringWithUTF8String:reason] : @"";
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!reconnect) {
            self.state = WASessionStateDisconnected;
            [[NSNotificationCenter defaultCenter] postNotificationName:WASessionStateChanged object:self userInfo:@{ @"state": @(WASessionStateDisconnected), @"detail": r }];
        }
    });
}

static void cb_device_changed(void *ctx, wa_device *d)
{
    wa_device_save(d, [storePath() UTF8String]);
}

@implementation WASession

+ (instancetype)shared
{
    static WASession *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[WASession alloc] init]; });
    return s;
}

- (instancetype)init
{
    if ((self = [super init])) {
        // Load the saved device up front so -registered is accurate at launch (WAAppDelegate decides Chats vs Pairing
        // before -start runs). Load-only: -start's loadOrCreateDevice reloads it or creates a new one if there is none.
        if (wa_device_load(&_dev, [storePath() UTF8String]) == 0 && _dev.registered)
            self.ownJID = [NSString stringWithUTF8String:_dev.pn];
    }
    return self;
}

- (BOOL)registered { return _dev.registered != 0; }

- (void)loadOrCreateDevice
{
    const char *path = [storePath() UTF8String];
    if (wa_device_load(&_dev, path) == 0) {
        WALog(@"loaded device (registered=%d)", _dev.registered);
        if (_dev.registered) self.ownJID = [NSString stringWithUTF8String:_dev.pn];
    } else {
        WALog(@"new device");
        if (wa_device_new(&_dev) != 0) { WALog(@"key generation failed"); return; }
        wa_device_save(&_dev, path);
    }
}

- (void)start
{
    if (self.running) return;
    [self loadOrCreateDevice];
    if (!_sessions) {
        _sessions = (wa_sess_store *)malloc(sizeof *_sessions);
        if (wa_sess_load(_sessions, [sessionsPath() UTF8String]) != 0) wa_sess_init(_sessions);
    }
    memset(&_cb, 0, sizeof _cb);
    _cb.ctx = (__bridge void *)self;
    _cb.on_qr = cb_qr;
    _cb.on_paired = cb_paired;
    _cb.on_connected = cb_connected;
    _cb.on_node = cb_node;
    _cb.on_disconnect = cb_disconnect;
    _cb.on_device_changed = cb_device_changed;
    memset(&_cfg, 0, sizeof _cfg);          // defaults: web.whatsapp.com, Noise authenticates WA
    _client = wa_client_new(&_dev, &_cb, &_cfg);
    self.running = YES;
    self.state = WASessionStateConnecting;
    _thread = [[NSThread alloc] initWithTarget:self selector:@selector(run) object:nil];
    _thread.name = @"wa6-client";
    [_thread start];
}

- (void)run
{
    @autoreleasepool {
        wa_client_run(_client);
        self.running = NO;
    }
}

- (void)stop
{
    if (_client) wa_client_stop(_client);
    self.running = NO;
}

- (void)unlinkAndReset
{
    [self stop];
    [[NSFileManager defaultManager] removeItemAtPath:storePath() error:NULL];
    [[NSFileManager defaultManager] removeItemAtPath:sessionsPath() error:NULL];
    memset(&_dev, 0, sizeof _dev);
    if (_sessions) wa_sess_init(_sessions);
    self.ownJID = nil;
    self.qrString = nil;
    self.state = WASessionStateDisconnected;
}

// --- actions that run on the client thread ---

typedef struct {
    void *session;
    char jid[80];
    char text[4096];
    char mid[64];
    int video;
} wa_action_ctx;

static void action_send_text(wa_client *c, void *ctx)
{
    wa_action_ctx *a = (wa_action_ctx *)ctx;
    WASession *self = (__bridge WASession *)a->session;
    char used_id[64] = {0};
    int rc = wa_chat_send_text_with_id(c, self->_sessions, a->jid, a->text, a->mid[0] ? a->mid : NULL, used_id, sizeof used_id);
    if (rc == 0) {
        wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
    } else {
        const char *fid = a->mid[0] ? a->mid : used_id;
        if (fid[0]) {
            NSString *mid = [NSString stringWithUTF8String:fid];
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus
                                                                    object:self
                                                                  userInfo:@{ @"id": mid, @"status": @(WA_STATUS_ERROR), @"error": @"send_failed" }];
            });
        }
    }
    free(a);
}

static void action_place_call(wa_client *c, void *ctx)
{
    wa_action_ctx *a = (wa_action_ctx *)ctx;
    WASession *self = (__bridge WASession *)a->session;
    wa_call_state st;
    if (wa_callengine_place(c, self->_sessions, a->jid, a->video, &st) == 0)
        wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
    free(a);
}

- (void)sendText:(NSString *)text toJID:(NSString *)jid
{
    [self sendText:text toJID:jid msgId:nil];
}

- (void)sendText:(NSString *)text toJID:(NSString *)jid msgId:(NSString *)mid
{
    if (!_client || !jid.length) return;
    wa_action_ctx *a = (wa_action_ctx *)calloc(1, sizeof *a);
    a->session = (__bridge void *)self;
    snprintf(a->jid, sizeof a->jid, "%s", jid.UTF8String);
    snprintf(a->text, sizeof a->text, "%s", text.UTF8String ?: "");
    if (mid.length) snprintf(a->mid, sizeof a->mid, "%s", mid.UTF8String);
    if (wa_client_post(_client, action_send_text, a) != 0) {
        if (mid.length) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus
                                                                    object:self
                                                                  userInfo:@{ @"id": mid, @"status": @(WA_STATUS_ERROR), @"error": @"post_failed" }];
            });
        }
        free(a);
    }
}

- (void)placeCallTo:(NSString *)jid video:(BOOL)video
{
    if (!_client || !jid.length) return;
    wa_action_ctx *a = (wa_action_ctx *)calloc(1, sizeof *a);
    a->session = (__bridge void *)self;
    snprintf(a->jid, sizeof a->jid, "%s", jid.UTF8String);
    a->video = video ? 1 : 0;
    if (wa_client_post(_client, action_place_call, a) != 0) free(a);
}

typedef struct {
    char jid[80];
    char state[32];
} wa_presence_ctx;

static void action_send_presence(wa_client *c, void *ctx)
{
    wa_presence_ctx *p = (wa_presence_ctx *)ctx;
    wa_node *n = wa_node_new("presence");
    wa_node_attr(n, "type", p->state);
    wa_node_attr(n, "to", p->jid);
    wa_client_send(c, n);
    wa_node_free(n);
    free(p);
}

static void action_send_chatstate(wa_client *c, void *ctx)
{
    wa_presence_ctx *p = (wa_presence_ctx *)ctx;
    wa_node *n = wa_node_new("chatstate");
    wa_node_attr(n, "to", p->jid);
    wa_node_add_new(n, p->state);
    wa_client_send(c, n);
    wa_node_free(n);
    free(p);
}

- (void)subscribePresence:(NSString *)jid
{
    if (!_client || !jid.length) return;
    wa_presence_ctx *p = (wa_presence_ctx *)calloc(1, sizeof *p);
    snprintf(p->jid, sizeof p->jid, "%s", jid.UTF8String);
    snprintf(p->state, sizeof p->state, "subscribe");
    if (wa_client_post(_client, action_send_presence, p) != 0) free(p);
}

- (void)unsubscribePresence:(NSString *)jid
{
    if (!_client || !jid.length) return;
    wa_presence_ctx *p = (wa_presence_ctx *)calloc(1, sizeof *p);
    snprintf(p->jid, sizeof p->jid, "%s", jid.UTF8String);
    snprintf(p->state, sizeof p->state, "unsubscribe");
    if (wa_client_post(_client, action_send_presence, p) != 0) free(p);
}

- (void)sendChatState:(BOOL)composing toJID:(NSString *)jid
{
    if (!_client || !jid.length) return;
    wa_presence_ctx *p = (wa_presence_ctx *)calloc(1, sizeof *p);
    snprintf(p->jid, sizeof p->jid, "%s", jid.UTF8String);
    snprintf(p->state, sizeof p->state, "%s", composing ? "composing" : "paused");
    if (wa_client_post(_client, action_send_chatstate, p) != 0) free(p);
}

- (void)dealloc { if (_sessions) free(_sessions); }

@end
