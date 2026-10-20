#import "WAStatusViewerViewController.h"
#import "WATheme.h"
#import <QuartzCore/QuartzCore.h>

@interface WAStatusViewerViewController ()
@property (nonatomic, copy) NSString *contactName;
@property (nonatomic, copy) NSString *jid;
@property (nonatomic, copy) NSString *timeText;
@property (nonatomic, copy) NSString *statusText;
@property (nonatomic, strong) UIImage *statusImage;
@property (nonatomic, strong) UIView *progressBar;
@property (nonatomic, strong) NSTimer *timer;
@property (nonatomic) NSTimeInterval startTime;
@end

@implementation WAStatusViewerViewController

- (instancetype)initWithContactName:(NSString *)name jid:(NSString *)jid timeText:(NSString *)timeText text:(NSString *)text image:(UIImage *)image
{
    if ((self = [super init])) {
        _contactName = [name copy];
        _jid = [jid copy];
        _timeText = [timeText copy];
        _statusText = [text copy];
        _statusImage = image;
    }
    return self;
}

- (BOOL)prefersStatusBarHidden { return YES; }

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];

    CGFloat w = self.view.bounds.size.width;
    CGFloat h = self.view.bounds.size.height;

    // Content view
    if (self.statusImage) {
        UIImageView *iv = [[UIImageView alloc] initWithFrame:self.view.bounds];
        iv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        iv.contentMode = UIViewContentModeScaleAspectFit;
        iv.image = self.statusImage;
        [self.view addSubview:iv];

        if (self.statusText.length) {
            UIView *capBg = [[UIView alloc] initWithFrame:CGRectMake(0, h - 80, w, 80)];
            capBg.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin;
            capBg.backgroundColor = [UIColor colorWithWhite:0 alpha:0.65];
            UILabel *cap = [[UILabel alloc] initWithFrame:CGRectMake(20, 10, w - 40, 60)];
            cap.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            cap.backgroundColor = [UIColor clearColor];
            cap.textColor = [UIColor whiteColor];
            cap.font = [WATheme bodyFont];
            cap.numberOfLines = 2;
            cap.textAlignment = NSTextAlignmentCenter;
            cap.text = self.statusText;
            [capBg addSubview:cap];
            [self.view addSubview:capBg];
        }
    } else {
        // Rich text gradient card
        UIView *card = [[UIView alloc] initWithFrame:self.view.bounds];
        card.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        CAGradientLayer *grad = [CAGradientLayer layer];
        grad.frame = self.view.bounds;
        grad.colors = @[ (id)[WATheme primaryActionColor].CGColor,
                         (id)[UIColor colorWithRed:0.05 green:0.25 blue:0.3 alpha:1.0].CGColor ];
        [card.layer addSublayer:grad];
        [self.view addSubview:card];

        UILabel *lbl = [[UILabel alloc] initWithFrame:CGRectMake(30, (h - 200) / 2, w - 60, 200)];
        lbl.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin;
        lbl.backgroundColor = [UIColor clearColor];
        lbl.textColor = [UIColor whiteColor];
        lbl.font = [WATheme headlineFont];
        lbl.numberOfLines = 0;
        lbl.textAlignment = NSTextAlignmentCenter;
        lbl.shadowColor = [UIColor colorWithWhite:0 alpha:0.6];
        lbl.shadowOffset = CGSizeMake(0, 1);
        lbl.text = self.statusText.length ? self.statusText : @"Ahoj! Používám Wazzup na iOS 6 ✨";
        [self.view addSubview:lbl];
    }

    // Top Bar (semi-transparent)
    UIView *topBar = [[UIView alloc] initWithFrame:CGRectMake(0, 0, w, 64)];
    topBar.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    topBar.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
    [self.view addSubview:topBar];

    // Progress bar track
    UIView *track = [[UIView alloc] initWithFrame:CGRectMake(12, 10, w - 24, 3)];
    track.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    track.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.35];
    track.layer.cornerRadius = 1.5;
    track.clipsToBounds = YES;
    [topBar addSubview:track];

    self.progressBar = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 0, 3)];
    self.progressBar.backgroundColor = [UIColor whiteColor];
    [track addSubview:self.progressBar];

    // Contact avatar
    UIImageView *av = [[UIImageView alloc] initWithFrame:CGRectMake(14, 20, 36, 36)];
    av.layer.cornerRadius = 18;
    av.clipsToBounds = YES;
    av.image = [WATheme avatarForName:self.contactName jid:self.jid diameter:36];
    [topBar addSubview:av];

    // Contact name
    UILabel *nl = [[UILabel alloc] initWithFrame:CGRectMake(58, 20, w - 120, 20)];
    nl.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    nl.backgroundColor = [UIColor clearColor];
    nl.font = [UIFont boldSystemFontOfSize:15];
    nl.textColor = [UIColor whiteColor];
    nl.text = self.contactName;
    [topBar addSubview:nl];

    // Time subtitle
    UILabel *tl = [[UILabel alloc] initWithFrame:CGRectMake(58, 40, w - 120, 14)];
    tl.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    tl.backgroundColor = [UIColor clearColor];
    tl.font = [UIFont systemFontOfSize:12];
    tl.textColor = [UIColor colorWithWhite:0.85 alpha:1.0];
    tl.text = self.timeText.length ? self.timeText : @"Před chvílí";
    [topBar addSubview:tl];

    // Close button
    UIButton *close = [UIButton buttonWithType:UIButtonTypeCustom];
    close.frame = CGRectMake(w - 46, 18, 40, 40);
    close.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin;
    [close setTitle:@"✕" forState:UIControlStateNormal];
    close.titleLabel.font = [UIFont boldSystemFontOfSize:20];
    [close setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    [close addTarget:self action:@selector(dismissSelf) forControlEvents:UIControlEventTouchUpInside];
    [topBar addSubview:close];

    // Tap gestures: tap to dismiss
    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(onViewTapped:)];
    [self.view addGestureRecognizer:tap];

    UISwipeGestureRecognizer *swipe = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(dismissSelf)];
    swipe.direction = UISwipeGestureRecognizerDirectionDown;
    [self.view addGestureRecognizer:swipe];
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    self.startTime = [NSDate timeIntervalSinceReferenceDate];
    self.timer = [NSTimer scheduledTimerWithTimeInterval:0.05 target:self selector:@selector(tick) userInfo:nil repeats:YES];
}

- (void)viewWillDisappear:(BOOL)animated
{
    [super viewWillDisappear:animated];
    [self.timer invalidate];
    self.timer = nil;
}

- (void)tick
{
    NSTimeInterval elapsed = [NSDate timeIntervalSinceReferenceDate] - self.startTime;
    CGFloat total = 5.0;
    CGFloat progress = MIN(elapsed / total, 1.0);
    CGFloat trackW = self.view.bounds.size.width - 24;
    self.progressBar.frame = CGRectMake(0, 0, trackW * progress, 3);
    if (progress >= 1.0) {
        [self.timer invalidate];
        self.timer = nil;
        [self dismissSelf];
    }
}

- (void)onViewTapped:(UITapGestureRecognizer *)g
{
    CGPoint p = [g locationInView:self.view];
    if (p.x < self.view.bounds.size.width * 0.3) {
        // Rewind
        self.startTime = [NSDate timeIntervalSinceReferenceDate];
    } else {
        // Next / dismiss
        [self dismissSelf];
    }
}

- (void)dismissSelf
{
    [self.timer invalidate];
    self.timer = nil;
    [self dismissViewControllerAnimated:YES completion:nil];
}

@end
