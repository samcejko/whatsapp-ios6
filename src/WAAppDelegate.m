#import "WAAppDelegate.h"
#import "WACommon.h"
#import "WASession.h"
#import "WAChatStore.h"
#import "UI/WATheme.h"
#import "UI/WAPairViewController.h"
#import "UI/WAChatsViewController.h"
#import "UI/WAChatViewController.h"
#import "UI/WASettingsViewController.h"
#import "UI/WAMediaViewController.h"
#import "UI/WACallViewController.h"
#import "WAChatStore.h"
#import <QuartzCore/QuartzCore.h>
#include "wa_history.h"

@implementation WAAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    self.window.backgroundColor = [UIColor whiteColor];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onPaired:) name:WASessionPaired object:nil];

    [WAChatStore shared];   // start observing history-sync + message notifications before the session connects

    if ([WASession shared].registered) [self showChats];
    else [self showPairing];

    [self.window makeKeyAndVisible];
    [[WASession shared] start];
    return YES;
}

- (void)onPaired:(NSNotification *)n
{
    // After pairing the server drops us (515) and the client reconnects logged-in; move to the chat list now.
    [self showChats];
}

- (void)setRoot:(UIViewController *)vc
{
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:vc];
    [WATheme styleNavigationBar:nav.navigationBar];
    self.window.rootViewController = nav;
}

- (void)showPairing { [self setRoot:[[WAPairViewController alloc] init]]; }
- (void)showChats { [self setRoot:[[WAChatsViewController alloc] init]]; }

// ---- debug URL scheme (wazzup://) for headless verification over SSH via `uiopen` ----
//   wazzup://shot            -> write a PNG of the current screen to Documents/wa6-shot.png
//   wazzup://open?jid=<jid>  -> pop to the chat list and open that conversation (so it can then be shot)
- (BOOL)handleDebugURL:(NSURL *)url
{
    NSString *cmd = url.host;
    if ([cmd isEqualToString:@"shot"]) {
        UIWindow *w = self.window;
        UIGraphicsBeginImageContextWithOptions(w.bounds.size, NO, 0);
        [w.layer renderInContext:UIGraphicsGetCurrentContext()];
        UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
        UIGraphicsEndImageContext();
        NSData *png = img ? UIImagePNGRepresentation(img) : nil;
        if (png) [png writeToFile:[WADocumentsPath() stringByAppendingPathComponent:@"wa6-shot.png"] atomically:YES];
        WALog(@"debug shot: %lu bytes", (unsigned long)png.length);
        return YES;
    }
    if ([cmd isEqualToString:@"open"]) {
        NSString *jid = nil;
        for (NSString *kv in [url.query componentsSeparatedByString:@"&"]) {
            NSArray *p = [kv componentsSeparatedByString:@"="];
            if (p.count == 2 && [p[0] isEqualToString:@"jid"])
                jid = [p[1] stringByReplacingPercentEscapesUsingEncoding:NSUTF8StringEncoding];
        }
        if (jid.length && [self.window.rootViewController isKindOfClass:[UINavigationController class]]) {
            UINavigationController *nav = (UINavigationController *)self.window.rootViewController;
            [nav popToRootViewControllerAnimated:NO];
            NSString *name = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
            [nav pushViewController:[[WAChatViewController alloc] initWithJID:jid name:name] animated:NO];
            WALog(@"debug open: %@", jid);
        }
        return YES;
    }
    if ([cmd isEqualToString:@"settings"] && [self.window.rootViewController isKindOfClass:[UINavigationController class]]) {
        UINavigationController *nav = (UINavigationController *)self.window.rootViewController;
        [nav popToRootViewControllerAnimated:NO];
        [nav pushViewController:[[WASettingsViewController alloc] init] animated:NO];
        return YES;
    }
    if ([cmd isEqualToString:@"photo"]) {
        NSString *jid = nil;
        for (NSString *kv in [url.query componentsSeparatedByString:@"&"]) {
            NSArray *p = [kv componentsSeparatedByString:@"="];
            if (p.count == 2 && [p[0] isEqualToString:@"jid"]) jid = [p[1] stringByReplacingPercentEscapesUsingEncoding:NSUTF8StringEncoding];
        }
        for (NSDictionary *m in [[WAChatStore shared] messagesForJID:jid]) {
            if ([m[@"kind"] intValue] == WA_MSG_IMAGE && m[@"thumb"]) {
                [self.window.rootViewController presentViewController:[[WAMediaViewController alloc] initWithMessage:m] animated:NO completion:nil];
                break;
            }
        }
        return YES;
    }
    if ([cmd isEqualToString:@"presence"]) {
        NSString *jid = nil, *state = @"available", *last = nil;
        for (NSString *kv in [url.query componentsSeparatedByString:@"&"]) {
            NSArray *p = [kv componentsSeparatedByString:@"="];
            if (p.count == 2) {
                NSString *k = p[0], *v = [p[1] stringByReplacingPercentEscapesUsingEncoding:NSUTF8StringEncoding];
                if ([k isEqualToString:@"jid"]) jid = v;
                else if ([k isEqualToString:@"state"]) state = v;
                else if ([k isEqualToString:@"last"]) last = v;
            }
        }
        if (jid.length) {
            NSMutableDictionary *ui = [NSMutableDictionary dictionaryWithObjectsAndKeys:jid, @"jid", state, @"state", nil];
            if (last.length) ui[@"last"] = last;
            [[NSNotificationCenter defaultCenter] postNotificationName:WASessionPresenceChanged object:nil userInfo:ui];
        }
        return YES;
    }
    if ([cmd isEqualToString:@"call"]) {
        NSString *name = @"Test", *jid = nil;
        BOOL video = NO, incoming = NO;
        for (NSString *kv in [url.query componentsSeparatedByString:@"&"]) {
            NSArray *p = [kv componentsSeparatedByString:@"="];
            if (p.count == 2) {
                NSString *k = p[0], *v = [p[1] stringByReplacingPercentEscapesUsingEncoding:NSUTF8StringEncoding];
                if ([k isEqualToString:@"name"]) name = v;
                else if ([k isEqualToString:@"jid"]) jid = v;
                else if ([k isEqualToString:@"video"]) video = [v boolValue];
                else if ([k isEqualToString:@"incoming"]) incoming = [v boolValue];
            }
        }
        WACallViewController *c = [[WACallViewController alloc] initWithJID:jid name:name video:video incoming:incoming];
        [self.window.rootViewController presentViewController:c animated:NO completion:nil];
        return YES;
    }
    return NO;
}

- (BOOL)application:(UIApplication *)application handleOpenURL:(NSURL *)url { return [self handleDebugURL:url]; }
- (BOOL)application:(UIApplication *)application openURL:(NSURL *)url sourceApplication:(NSString *)src annotation:(id)ann { return [self handleDebugURL:url]; }

- (void)applicationDidEnterBackground:(UIApplication *)application
{
    // Keep the VoIP connection alive in the background so incoming calls still ring (best-effort on iOS 6).
    WALog(@"entered background; connection kept for calls");
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

@end
