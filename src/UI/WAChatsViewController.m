#import "WAChatsViewController.h"
#import "WAChatViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"
#import "../WAAppDelegate.h"

@interface WAChatsViewController ()
@property (nonatomic, strong) UITableView *table;
@property (nonatomic, strong) UILabel *empty;
@property (nonatomic, strong) UILabel *banner;
@property (nonatomic, strong) NSMutableArray *chats;   // array of @{ @"jid", @"name", @"last" }
@end

@implementation WAChatsViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = WALoc(@"chats_title");
    self.chats = [NSMutableArray array];
    self.view.backgroundColor = [UIColor whiteColor];

    self.navigationItem.leftBarButtonItem =
        [[UIBarButtonItem alloc] initWithTitle:WALoc(@"unlink") style:UIBarButtonItemStylePlain target:self action:@selector(unlink)];
    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(newChat)];

    self.table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStylePlain];
    self.table.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.rowHeight = 64;
    [self.view addSubview:self.table];

    self.banner = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, self.view.bounds.size.width, 28)];
    self.banner.backgroundColor = [WATheme accent];
    self.banner.textColor = [UIColor whiteColor];
    self.banner.textAlignment = NSTextAlignmentCenter;
    self.banner.font = [UIFont systemFontOfSize:13];
    self.banner.text = WALoc(@"status_connecting");

    self.empty = [[UILabel alloc] init];
    self.empty.numberOfLines = 0;
    self.empty.textAlignment = NSTextAlignmentCenter;
    self.empty.textColor = [UIColor colorWithWhite:0.5 alpha:1.0];
    self.empty.font = [UIFont systemFontOfSize:15];
    self.empty.text = WALoc(@"no_chats");
    [self.view addSubview:self.empty];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onState:) name:WASessionStateChanged object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onMessage:) name:WASessionMessage object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onConversations:) name:WASessionConversations object:nil];
    [self refreshBanner:[WASession shared].state];
}

// A display name for a chat: the history/contact name when we have one, else the number before the "@".
- (NSString *)displayNameFor:(NSString *)jid name:(NSString *)name
{
    if (name.length) return name;
    return [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
}

// Upsert one chat row (keyed by jid). Does NOT reload; call reloadSorted after a batch.
- (void)upsertJID:(NSString *)jid name:(NSString *)name last:(NSString *)last ts:(unsigned long long)ts
{
    if (!jid.length) return;
    int found = -1;
    for (NSUInteger i = 0; i < self.chats.count; i++) if ([self.chats[i][@"jid"] isEqualToString:jid]) { found = (int)i; break; }
    NSDictionary *row = @{ @"jid": jid,
                          @"name": [self displayNameFor:jid name:name],
                          @"last": last ?: @"",
                          @"ts": @(ts) };
    if (found >= 0) [self.chats replaceObjectAtIndex:found withObject:row];
    else [self.chats addObject:row];
}

// Sort newest-first by timestamp (0 timestamps keep their relative order at the bottom), then redraw.
- (void)reloadSorted
{
    [self.chats sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        unsigned long long ta = [a[@"ts"] unsignedLongLongValue], tb = [b[@"ts"] unsignedLongLongValue];
        if (ta == tb) return NSOrderedSame;
        return ta > tb ? NSOrderedAscending : NSOrderedDescending;
    }];
    self.empty.hidden = self.chats.count > 0;
    [self.table reloadData];
}

- (void)onMessage:(NSNotification *)n
{
    NSString *from = n.userInfo[@"from"] ?: @"";
    NSString *text = n.userInfo[@"text"] ?: @"";
    // A live message is the newest thing in that chat: stamp it with "now" so it sorts to the top.
    [self upsertJID:from name:nil last:text ts:(unsigned long long)[[NSDate date] timeIntervalSince1970]];
    [self reloadSorted];
}

// A history-sync chunk: fill/merge the chat list with the user's existing conversations.
- (void)onConversations:(NSNotification *)n
{
    NSArray *convs = n.userInfo[@"conversations"];
    if (![convs isKindOfClass:[NSArray class]]) return;
    for (NSDictionary *c in convs) {
        [self upsertJID:c[@"jid"] name:c[@"name"] last:c[@"last"] ts:[c[@"ts"] unsignedLongLongValue]];
    }
    [self reloadSorted];
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    self.empty.frame = CGRectMake(30, 120, self.view.bounds.size.width - 60, 80);
}

- (void)refreshBanner:(WASessionState)s
{
    NSString *t = WALoc(@"status_offline");
    UIColor *col = [UIColor colorWithWhite:0.5 alpha:1.0];
    if (s == WASessionStateConnecting) t = WALoc(@"status_connecting");
    else if (s == WASessionStateHandshaking) t = WALoc(@"status_handshaking");
    else if (s == WASessionStateConnected) { t = WALoc(@"status_connected"); col = [WATheme accent]; }
    else if (s == WASessionStatePairing) t = WALoc(@"status_pairing");
    self.banner.text = t;
    self.banner.backgroundColor = col;
    self.table.tableHeaderView = (s == WASessionStateConnected) ? nil : self.banner;
    self.empty.hidden = self.chats.count > 0;
}

- (void)onState:(NSNotification *)n { [self refreshBanner:[n.userInfo[@"state"] integerValue]]; }

- (void)unlink
{
    UIAlertView *a = [[UIAlertView alloc] initWithTitle:WALoc(@"app_name") message:WALoc(@"unlink_confirm")
                                               delegate:self cancelButtonTitle:WALoc(@"cancel") otherButtonTitles:WALoc(@"unlink"), nil];
    a.tag = 1;
    [a show];
}

- (void)alertView:(UIAlertView *)alertView clickedButtonAtIndex:(NSInteger)buttonIndex
{
    if (alertView.tag == 1 && buttonIndex == 1) {
        [[WASession shared] unlinkAndReset];
        WAAppDelegate *ad = (WAAppDelegate *)[UIApplication sharedApplication].delegate;
        [ad showPairing];
    } else if (alertView.tag == 2 && buttonIndex == 1) {
        UITextField *tf = [alertView textFieldAtIndex:0];
        NSString *num = [tf.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (num.length) [self openChatWithNumber:num];
    }
}

- (void)newChat
{
    UIAlertView *a = [[UIAlertView alloc] initWithTitle:WALoc(@"chats_title") message:@"+country number"
                                               delegate:self cancelButtonTitle:WALoc(@"cancel") otherButtonTitles:@"OK", nil];
    a.tag = 2;
    a.alertViewStyle = UIAlertViewStylePlainTextInput;
    [[a textFieldAtIndex:0] setKeyboardType:UIKeyboardTypePhonePad];
    [a show];
}

- (void)openChatWithNumber:(NSString *)num
{
    NSString *digits = [[num componentsSeparatedByCharactersInSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]] componentsJoinedByString:@""];
    if (!digits.length) return;
    NSString *jid = [digits stringByAppendingString:@"@s.whatsapp.net"];
    WAChatViewController *c = [[WAChatViewController alloc] initWithJID:jid name:num];
    [self.navigationController pushViewController:c animated:YES];
}

#pragma mark - table

- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return self.chats.count; }

- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip
{
    UITableViewCell *cell = [t dequeueReusableCellWithIdentifier:@"c"];
    if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"c"];
    NSDictionary *chat = self.chats[ip.row];
    cell.textLabel.text = chat[@"name"];
    cell.detailTextLabel.text = chat[@"last"];
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)ip
{
    [t deselectRowAtIndexPath:ip animated:YES];
    NSDictionary *chat = self.chats[ip.row];
    WAChatViewController *c = [[WAChatViewController alloc] initWithJID:chat[@"jid"] name:chat[@"name"]];
    [self.navigationController pushViewController:c animated:YES];
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

@end
