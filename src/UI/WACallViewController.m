#import "WACallViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"
#import "../WAAudioEngine.h"
#import <QuartzCore/QuartzCore.h>

@interface WACallViewController ()
@property (nonatomic, copy) NSString *jid;
@property (nonatomic, copy) NSString *name;
@property (nonatomic) BOOL video;
@property (nonatomic) BOOL incoming;
@property (nonatomic) BOOL connected;
@property (nonatomic) BOOL muted;
@property (nonatomic) BOOL speaker;
@property (nonatomic, strong) UILabel *nameLabel;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, strong) UIView *avatar;
@property (nonatomic, strong) NSMutableArray *buttons;
@property (nonatomic, strong) NSTimer *timer;
@property (nonatomic) NSTimeInterval startTime;
@property (nonatomic, strong) WAAudioEngine *audio;
@end

@implementation WACallViewController

- (instancetype)initWithName:(NSString *)name video:(BOOL)video incoming:(BOOL)incoming
{
    return [self initWithJID:nil name:name video:video incoming:incoming];
}

- (instancetype)initWithJID:(NSString *)jid name:(NSString *)name video:(BOOL)video incoming:(BOOL)incoming
{
    if ((self = [super init])) {
        _jid = [jid copy];
        _name = [name copy];
        _video = video;
        _incoming = incoming;
    }
    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    [self drawBackground];

    self.avatar = [[UIView alloc] init];
    self.avatar.layer.cornerRadius = 55;
    self.avatar.clipsToBounds = YES;
    [self.view addSubview:self.avatar];
    UIImageView *avImg = [[UIImageView alloc] initWithFrame:CGRectMake(0, 0, 110, 110)];
    avImg.image = [WATheme avatarForName:self.name jid:self.jid diameter:110];
    [self.avatar addSubview:avImg];

    self.nameLabel = [[UILabel alloc] init];
    self.nameLabel.textAlignment = NSTextAlignmentCenter;
    self.nameLabel.font = [UIFont systemFontOfSize:28];
    self.nameLabel.textColor = [UIColor whiteColor];
    self.nameLabel.backgroundColor = [UIColor clearColor];
    self.nameLabel.text = self.name;
    [self.view addSubview:self.nameLabel];

    self.statusLabel = [[UILabel alloc] init];
    self.statusLabel.textAlignment = NSTextAlignmentCenter;
    self.statusLabel.font = [WATheme bodyFont];
    self.statusLabel.textColor = [UIColor whiteColor];
    self.statusLabel.backgroundColor = [UIColor clearColor];
    self.statusLabel.text = self.incoming ? (self.video ? WALoc(@"call_incoming_video") : WALoc(@"call_incoming"))
                                          : WALoc(@"call_outgoing");
    [self.view addSubview:self.statusLabel];

    self.buttons = [NSMutableArray array];
    if (self.incoming) {
        [self addButton:WALoc(@"call_decline") color:[WATheme errorColor] glyph:@"✕" action:@selector(hangup)];
        [self addButton:WALoc(@"call_accept") color:[WATheme successColor] glyph:@"✓" action:@selector(accept)];
    } else {
        [self addButton:WALoc(@"call_mute") color:[UIColor colorWithWhite:1 alpha:0.2] glyph:@"M" action:@selector(toggleMute)];
        [self addButton:WALoc(@"call_end") color:[WATheme errorColor] glyph:@"✕" action:@selector(hangup)];
        [self addButton:(self.video ? WALoc(@"call_camera") : WALoc(@"call_speaker")) color:[UIColor colorWithWhite:1 alpha:0.2] glyph:(self.video ? @"▶" : @"◉") action:(self.video ? @selector(noop) : @selector(toggleSpeaker))];
    }

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onCallRinging:) name:WASessionCallRinging object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onCallAccepted:) name:WASessionCallAccepted object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onCallEnded:) name:WASessionCallEnded object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onCallAudioPacket:) name:WASessionCallAudioPacket object:nil];
}

- (void)drawBackground
{
    CAGradientLayer *g = [CAGradientLayer layer];
    g.frame = self.view.bounds;
    UIColor *top = [WATheme primaryActionColor];
    UIColor *bot = [UIColor colorWithRed:0.02 green:0.12 blue:0.11 alpha:1.0];
    g.colors = @[ (id)top.CGColor, (id)bot.CGColor ];
    [self.view.layer insertSublayer:g atIndex:0];
    self.view.backgroundColor = bot;
}

- (void)addButton:(NSString *)title color:(UIColor *)color glyph:(NSString *)glyph action:(SEL)action
{
    UIButton *b = [UIButton buttonWithType:UIButtonTypeCustom];
    [b setImage:[WATheme roundCallButton:color diameter:72 glyph:glyph] forState:UIControlStateNormal];
    [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    UILabel *cap = [[UILabel alloc] init];
    cap.text = title;
    cap.font = [WATheme captionFont];
    cap.textColor = [UIColor colorWithWhite:1 alpha:0.75];
    cap.textAlignment = NSTextAlignmentCenter;
    cap.backgroundColor = [UIColor clearColor];
    cap.tag = 99;
    [b addSubview:cap];
    [self.view addSubview:b];
    [self.buttons addObject:b];
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    CGFloat w = self.view.bounds.size.width, h = self.view.bounds.size.height;
    self.avatar.frame = CGRectMake((w - 110) / 2, h * 0.16, 110, 110);
    self.nameLabel.frame = CGRectMake(20, h * 0.16 + 128, w - 40, 34);
    self.statusLabel.frame = CGRectMake(20, h * 0.16 + 166, w - 40, 22);
    NSInteger n = self.buttons.count;
    CGFloat bd = 72, gap = (w - n * bd) / (n + 1);
    CGFloat y = h - bd - 48;
    for (NSInteger i = 0; i < n; i++) {
        UIButton *b = self.buttons[i];
        b.frame = CGRectMake(gap + i * (bd + gap), y, bd, bd);
        UILabel *cap = (UILabel *)[b viewWithTag:99];
        cap.frame = CGRectMake(-10, bd + 4, bd + 20, 16);
    }
}

- (void)onCallRinging:(NSNotification *)n
{
    if (self.connected) return;
    self.statusLabel.text = WALoc(@"call_ringing");
}

- (void)onCallAccepted:(NSNotification *)n
{
    if (self.connected) return;
    WALog(@"call accepted by remote peer!");
    [self connect];
}

- (void)onCallEnded:(NSNotification *)n
{
    WALog(@"call ended by remote peer");
    [self.timer invalidate];
    self.timer = nil;
    [self.audio stop];
    self.audio = nil;
    NSString *reason = n.userInfo[@"reason"];
    if ([reason isEqualToString:@"rejected"] || [reason isEqualToString:@"busy"]) {
        self.statusLabel.text = WALoc(@"call_declined");
    } else {
        self.statusLabel.text = WALoc(@"call_ended");
    }
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    [self performSelector:@selector(dismiss) withObject:nil afterDelay:1.0];
}

- (void)onCallAudioPacket:(NSNotification *)n
{
    NSData *payload = n.userInfo[@"payload"];
    if (payload.length && self.audio && self.connected) {
        [self.audio playFrame:payload];
    }
}

- (void)connect
{
    self.connected = YES;
    self.statusLabel.text = WALoc(@"call_connected");
    self.startTime = [NSDate timeIntervalSinceReferenceDate];
    self.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(tick) userInfo:nil repeats:YES];
    [self tick];
    // Open the mic/speaker (echo-cancelled, 16 kHz Opus)
    self.audio = [[WAAudioEngine alloc] init];
    self.audio.onFrame = ^(NSData *frame) {
        [[WASession shared] sendCallAudioFrame:frame];
    };
    [self.audio start];
}

- (void)tick
{
    int secs = (int)([NSDate timeIntervalSinceReferenceDate] - self.startTime);
    self.statusLabel.text = [NSString stringWithFormat:@"%d:%02d", secs / 60, secs % 60];
}

- (void)accept {
    [[WASession shared] acceptCall];
    self.incoming = NO;
    [self.buttons makeObjectsPerformSelector:@selector(removeFromSuperview)];
    [self.buttons removeAllObjects];
    [self addButton:WALoc(@"call_mute") color:[UIColor colorWithWhite:1 alpha:0.2] glyph:@"M" action:@selector(toggleMute)];
    [self addButton:WALoc(@"call_end") color:[WATheme errorColor] glyph:@"✕" action:@selector(hangup)];
    [self addButton:WALoc(@"call_speaker") color:[UIColor colorWithWhite:1 alpha:0.2] glyph:@"◉" action:@selector(toggleSpeaker)];
    [self viewDidLayoutSubviews];
    [self connect];
}

- (void)toggleMute { self.muted = !self.muted; self.audio.muted = self.muted; WALog(@"call mute=%d", self.muted); }
- (void)toggleSpeaker { self.speaker = !self.speaker; self.audio.speaker = self.speaker; WALog(@"call speaker=%d", self.speaker); }
- (void)noop {}

- (void)hangup
{
    if (self.incoming && !self.connected) {
        [[WASession shared] rejectCall];
    } else {
        [[WASession shared] terminateCall];
    }
    [self.timer invalidate];
    self.timer = nil;
    [self.audio stop];
    self.audio = nil;
    self.statusLabel.text = WALoc(@"call_ended");
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    [self performSelector:@selector(dismiss) withObject:nil afterDelay:0.5];
}

- (void)dismiss { [self dismissViewControllerAnimated:YES completion:nil]; }

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self.timer invalidate];
    [self.audio stop];
}

@end
