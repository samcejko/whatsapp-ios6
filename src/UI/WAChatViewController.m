#import "WAChatViewController.h"
#import "WACallViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"

@interface WAChatViewController ()
@property (nonatomic, copy) NSString *jid;
@property (nonatomic, strong) NSMutableArray *messages;   // @{ @"text", @"out" }
@property (nonatomic, strong) UITableView *table;
@property (nonatomic, strong) UIView *bar;
@property (nonatomic, strong) UITextField *field;
@property (nonatomic, strong) UIButton *send;
@property (nonatomic) CGFloat kbHeight;
@end

@implementation WAChatViewController

- (instancetype)initWithJID:(NSString *)jid name:(NSString *)name
{
    if ((self = [super init])) {
        _jid = [jid copy];
        self.title = name;
        _messages = [NSMutableArray array];
    }
    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [[UIColor alloc] initWithPatternImage:[WATheme wallpaper]];

    UIButton *call = [UIButton buttonWithType:UIButtonTypeCustom];
    [call setImage:[WATheme roundCallButton:[WATheme accent] diameter:30 glyph:@"☎"] forState:UIControlStateNormal];
    call.frame = CGRectMake(0, 0, 30, 30);
    [call addTarget:self action:@selector(voiceCall) forControlEvents:UIControlEventTouchUpInside];
    UIButton *video = [UIButton buttonWithType:UIButtonTypeCustom];
    [video setImage:[WATheme roundCallButton:[WATheme accent] diameter:30 glyph:@"▶"] forState:UIControlStateNormal];
    video.frame = CGRectMake(0, 0, 30, 30);
    [video addTarget:self action:@selector(videoCall) forControlEvents:UIControlEventTouchUpInside];
    self.navigationItem.rightBarButtonItems = @[ [[UIBarButtonItem alloc] initWithCustomView:video],
                                                 [[UIBarButtonItem alloc] initWithCustomView:call] ];

    self.table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStylePlain];
    self.table.backgroundColor = [UIColor clearColor];
    self.table.separatorStyle = UITableViewCellSeparatorStyleNone;
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.allowsSelection = NO;
    [self.view addSubview:self.table];

    self.bar = [[UIView alloc] init];
    self.bar.backgroundColor = [UIColor colorWithWhite:0.93 alpha:1.0];
    [self.view addSubview:self.bar];

    self.field = [[UITextField alloc] init];
    self.field.borderStyle = UITextBorderStyleRoundedRect;
    self.field.placeholder = WALoc(@"message_hint");
    self.field.font = [UIFont systemFontOfSize:16];
    self.field.returnKeyType = UIReturnKeySend;
    self.field.delegate = self;
    [self.bar addSubview:self.field];

    self.send = [UIButton buttonWithType:UIButtonTypeCustom];
    [self.send setTitle:WALoc(@"send") forState:UIControlStateNormal];
    [self.send setBackgroundImage:[WATheme glossyButtonWithColor:[WATheme accent]] forState:UIControlStateNormal];
    self.send.titleLabel.font = [UIFont boldSystemFontOfSize:15];
    [self.send addTarget:self action:@selector(sendTapped) forControlEvents:UIControlEventTouchUpInside];
    [self.bar addSubview:self.send];

    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(kbShow:) name:UIKeyboardWillShowNotification object:nil];
    [nc addObserver:self selector:@selector(kbHide:) name:UIKeyboardWillHideNotification object:nil];
    [nc addObserver:self selector:@selector(onMessage:) name:WASessionMessage object:nil];
}

// match two JIDs by their user (digits before @ / :)
static BOOL sameUser(NSString *a, NSString *b)
{
    NSString *ua = [[a componentsSeparatedByCharactersInSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]] componentsJoinedByString:@""];
    NSString *ub = [[b componentsSeparatedByCharactersInSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]] componentsJoinedByString:@""];
    return ua.length && [ua isEqualToString:ub];
}

- (void)onMessage:(NSNotification *)n
{
    if (!sameUser(n.userInfo[@"from"], self.jid)) return;
    [self.messages addObject:@{ @"text": n.userInfo[@"text"] ?: @"", @"out": @NO }];
    [self.table reloadData];
    [self scrollToBottom];
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    CGFloat w = self.view.bounds.size.width, h = self.view.bounds.size.height;
    CGFloat barH = 46;
    self.bar.frame = CGRectMake(0, h - barH - self.kbHeight, w, barH);
    self.field.frame = CGRectMake(8, 7, w - 8 - 70 - 8, barH - 14);
    self.send.frame = CGRectMake(w - 70 - 8, 7, 70, barH - 14);
    self.table.frame = CGRectMake(0, 0, w, h - barH - self.kbHeight);
}

- (void)kbShow:(NSNotification *)n
{
    CGRect r = [n.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    self.kbHeight = [self.view convertRect:r fromView:nil].size.height;
    [UIView animateWithDuration:0.25 animations:^{ [self viewDidLayoutSubviews]; }];
}

- (void)kbHide:(NSNotification *)n
{
    self.kbHeight = 0;
    [UIView animateWithDuration:0.25 animations:^{ [self viewDidLayoutSubviews]; }];
}

- (BOOL)textFieldShouldReturn:(UITextField *)tf { [self sendTapped]; return NO; }

- (void)sendTapped
{
    NSString *text = [self.field.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (!text.length) return;
    [self.messages addObject:@{ @"text": text, @"out": @YES }];
    self.field.text = @"";
    [self.table reloadData];
    [self scrollToBottom];
    [[WASession shared] sendText:text toJID:self.jid];
    WALog(@"send to %@: %lu chars", self.jid, (unsigned long)text.length);
}

- (void)scrollToBottom
{
    if (self.messages.count == 0) return;
    NSIndexPath *ip = [NSIndexPath indexPathForRow:self.messages.count - 1 inSection:0];
    [self.table scrollToRowAtIndexPath:ip atScrollPosition:UITableViewScrollPositionBottom animated:YES];
}

- (void)voiceCall
{
    [[WASession shared] placeCallTo:self.jid video:NO];
    WACallViewController *c = [[WACallViewController alloc] initWithName:self.title video:NO incoming:NO];
    c.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
    [self presentViewController:c animated:YES completion:nil];
}

- (void)videoCall
{
    [[WASession shared] placeCallTo:self.jid video:YES];
    WACallViewController *c = [[WACallViewController alloc] initWithName:self.title video:YES incoming:NO];
    c.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
    [self presentViewController:c animated:YES completion:nil];
}

#pragma mark - table (bubbles)

- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return self.messages.count; }

- (CGFloat)tableView:(UITableView *)t heightForRowAtIndexPath:(NSIndexPath *)ip
{
    NSString *text = self.messages[ip.row][@"text"];
    CGSize max = CGSizeMake(self.view.bounds.size.width * 0.72 - 24, 10000);
    CGSize s = [text sizeWithFont:[UIFont systemFontOfSize:16] constrainedToSize:max lineBreakMode:NSLineBreakByWordWrapping];
    return s.height + 26;
}

- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip
{
    UITableViewCell *cell = [t dequeueReusableCellWithIdentifier:@"b"];
    if (!cell) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"b"];
        cell.backgroundColor = [UIColor clearColor];
        cell.contentView.backgroundColor = [UIColor clearColor];
    }
    for (UIView *v in cell.contentView.subviews) [v removeFromSuperview];
    NSDictionary *m = self.messages[ip.row];
    BOOL out = [m[@"out"] boolValue];
    NSString *text = m[@"text"];
    CGFloat w = self.view.bounds.size.width;
    CGSize max = CGSizeMake(w * 0.72 - 24, 10000);
    CGSize ts = [text sizeWithFont:[UIFont systemFontOfSize:16] constrainedToSize:max lineBreakMode:NSLineBreakByWordWrapping];
    CGFloat bw = ts.width + 24, bh = ts.height + 16;
    CGRect bf = out ? CGRectMake(w - bw - 8, 6, bw, bh) : CGRectMake(8, 6, bw, bh);
    UIImageView *bubble = [[UIImageView alloc] initWithImage:[WATheme bubbleOutgoing:out]];
    bubble.frame = bf;
    [cell.contentView addSubview:bubble];
    UILabel *lbl = [[UILabel alloc] initWithFrame:CGRectMake(bf.origin.x + 12, bf.origin.y + 7, ts.width + 2, ts.height)];
    lbl.numberOfLines = 0;
    lbl.backgroundColor = [UIColor clearColor];
    lbl.font = [UIFont systemFontOfSize:16];
    lbl.text = text;
    [cell.contentView addSubview:lbl];
    return cell;
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

@end
