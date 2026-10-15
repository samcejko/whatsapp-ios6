#import "WAAppDelegate.h"
#import "WACommon.h"
#import "WASession.h"
#import "UI/WATheme.h"
#import "UI/WAPairViewController.h"
#import "UI/WAChatsViewController.h"

@implementation WAAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    self.window.backgroundColor = [UIColor whiteColor];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onPaired:) name:WASessionPaired object:nil];

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

- (void)applicationDidEnterBackground:(UIApplication *)application
{
    // Keep the VoIP connection alive in the background so incoming calls still ring (best-effort on iOS 6).
    WALog(@"entered background; connection kept for calls");
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

@end
