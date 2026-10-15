#import "WASession.h"
#import "WACommon.h"

#include "wa_store.h"
#include "wa_client.h"

NSString *const WASessionStateChanged = @"WASessionStateChanged";
NSString *const WASessionQRChanged = @"WASessionQRChanged";
NSString *const WASessionPaired = @"WASessionPaired";
NSString *const WASessionNodeReceived = @"WASessionNodeReceived";

@interface WASession () {
    wa_device _dev;
    wa_client *_client;
    wa_client_callbacks _cb;
    wa_client_config _cfg;
    NSThread *_thread;
}
@property (nonatomic) WASessionState state;
@property (nonatomic, copy) NSString *qrString;
@property (nonatomic, copy) NSString *ownJID;
@property (nonatomic) BOOL running;
@end

static NSString *storePath(void) { return [WADocumentsPath() stringByAppendingPathComponent:@"wa6.store"]; }

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

static void cb_node(void *ctx, const wa_node *node)
{
    WASession *self = (__bridge WASession *)ctx;
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
    memset(&_dev, 0, sizeof _dev);
    self.ownJID = nil;
    self.qrString = nil;
    self.state = WASessionStateDisconnected;
}

@end
