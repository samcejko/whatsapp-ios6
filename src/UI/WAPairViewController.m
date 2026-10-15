#import "WAPairViewController.h"
#import "WAQRView.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"

@interface WAPairViewController ()
@property (nonatomic, strong) WAQRView *qr;
@property (nonatomic, strong) UILabel *status;
@property (nonatomic, strong) UIActivityIndicatorView *spinner;
@end

@implementation WAPairViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = WALoc(@"pair_title");
    self.view.backgroundColor = [UIColor colorWithRed:0.93 green:0.93 blue:0.90 alpha:1.0];

    UILabel *intro = [[UILabel alloc] init];
    intro.numberOfLines = 0;
    intro.textAlignment = NSTextAlignmentCenter;
    intro.font = [UIFont systemFontOfSize:15];
    intro.textColor = [UIColor colorWithWhite:0.25 alpha:1.0];
    intro.backgroundColor = [UIColor clearColor];
    intro.text = WALoc(@"pair_instructions");
    [self.view addSubview:intro];

    self.qr = [[WAQRView alloc] init];
    self.qr.layer.borderColor = [UIColor colorWithWhite:0.8 alpha:1.0].CGColor;
    self.qr.layer.borderWidth = 1;
    [self.view addSubview:self.qr];

    self.spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleGray];
    [self.view addSubview:self.spinner];

    self.status = [[UILabel alloc] init];
    self.status.textAlignment = NSTextAlignmentCenter;
    self.status.font = [UIFont systemFontOfSize:14];
    self.status.textColor = [UIColor colorWithWhite:0.4 alpha:1.0];
    self.status.backgroundColor = [UIColor clearColor];
    self.status.text = WALoc(@"pair_generating");
    [self.view addSubview:self.status];
    [self.spinner startAnimating];

    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(onQR:) name:WASessionQRChanged object:nil];
    [nc addObserver:self selector:@selector(onState:) name:WASessionStateChanged object:nil];

    NSString *existing = [WASession shared].qrString;
    if (existing) { self.qr.text = existing; [self showWaiting]; }
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    CGFloat w = self.view.bounds.size.width, h = self.view.bounds.size.height;
    CGFloat top = 74;
    CGFloat side = MIN(w - 60, 260);
    [self.view.subviews[0] setFrame:CGRectMake(20, top, w - 40, 72)];     // intro
    self.qr.frame = CGRectMake((w - side) / 2, top + 84, side, side);
    self.spinner.center = CGPointMake(w / 2, top + 84 + side + 28);
    self.status.frame = CGRectMake(20, top + 84 + side + 44, w - 40, 24);
    (void)h;
}

- (void)showWaiting
{
    self.status.text = WALoc(@"pair_waiting");
    [self.spinner stopAnimating];
}

- (void)onQR:(NSNotification *)n
{
    self.qr.text = n.userInfo[@"qr"];
    [self showWaiting];
}

- (void)onState:(NSNotification *)n
{
    WASessionState s = [n.userInfo[@"state"] integerValue];
    if (s == WASessionStateConnecting) self.status.text = WALoc(@"status_connecting");
    else if (s == WASessionStateHandshaking) self.status.text = WALoc(@"status_handshaking");
    else if (s == WASessionStatePairing && !self.qr.text) self.status.text = WALoc(@"pair_generating");
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

@end
