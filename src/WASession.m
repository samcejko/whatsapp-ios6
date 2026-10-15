#import "WASession.h"
#import "WACommon.h"

#include "wa_store.h"
#include "wa_client.h"
#include "wa_sessions.h"
#include "wa_chat.h"
#include "wa_callengine.h"

NSString *const WASessionStateChanged = @"WASessionStateChanged";
NSString *const WASessionQRChanged = @"WASessionQRChanged";
NSString *const WASessionPaired = @"WASessionPaired";
NSString *const WASessionNodeReceived = @"WASessionNodeReceived";
NSString *const WASessionMessage = @"WASessionMessage";

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
        char *text = wa_chat_handle_message(self->_client, self->_sessions, node, from, sizeof from);
        wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
        send_ack(self, node, "message");
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

typedef struct { void *session; char jid[80]; char text[4096]; int video; } wa_action_ctx;

static void action_send_text(wa_client *c, void *ctx)
{
    wa_action_ctx *a = (wa_action_ctx *)ctx;
    WASession *self = (__bridge WASession *)a->session;
    if (wa_chat_send_text(c, self->_sessions, a->jid, a->text) == 0)
        wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
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
    if (!_client || !jid.length) return;
    wa_action_ctx *a = (wa_action_ctx *)calloc(1, sizeof *a);
    a->session = (__bridge void *)self;
    snprintf(a->jid, sizeof a->jid, "%s", jid.UTF8String);
    snprintf(a->text, sizeof a->text, "%s", text.UTF8String ?: "");
    if (wa_client_post(_client, action_send_text, a) != 0) free(a);
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

- (void)dealloc { if (_sessions) free(_sessions); }

@end
