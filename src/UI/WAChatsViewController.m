#import "WAChatsViewController.h"
#import "WAChatViewController.h"
#import "WASettingsViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"
#import "../WAChatStore.h"
#import "../WAAppDelegate.h"

// A WhatsApp-style chat-list row: avatar, name, last-message preview, time, unread badge.
@interface WAChatCell : UITableViewCell
@property (nonatomic, strong) UIImageView *avatar;
@property (nonatomic, strong) UILabel *nameLbl;
@property (nonatomic, strong) UILabel *lastLbl;
@property (nonatomic, strong) UILabel *timeLbl;
@property (nonatomic, strong) UIImageView *badge;
@end

@implementation WAChatCell
- (instancetype)initWithStyle:(UITableViewCellStyle)style reuseIdentifier:(NSString *)rid
{
    if ((self = [super initWithStyle:style reuseIdentifier:rid])) {
        _avatar = [[UIImageView alloc] init];
        _nameLbl = [[UILabel alloc] init];
        _nameLbl.font = [UIFont boldSystemFontOfSize:17];
        _lastLbl = [[UILabel alloc] init];
        _lastLbl.font = [UIFont systemFontOfSize:14];
        _lastLbl.textColor = [UIColor colorWithWhite:0.52 alpha:1.0];
        _timeLbl = [[UILabel alloc] init];
        _timeLbl.font = [UIFont systemFontOfSize:12];
        _timeLbl.textColor = [UIColor colorWithWhite:0.6 alpha:1.0];
        _timeLbl.textAlignment = NSTextAlignmentRight;
        _badge = [[UIImageView alloc] init];
        for (UIView *v in @[ _avatar, _nameLbl, _lastLbl, _timeLbl, _badge ]) [self.contentView addSubview:v];
    }
    return self;
}
- (void)layoutSubviews
{
    [super layoutSubviews];
    CGFloat w = self.contentView.bounds.size.width, h = self.contentView.bounds.size.height, av = 52;
    _avatar.frame = CGRectMake(10, (h - av) / 2, av, av);
    CGFloat lx = 10 + av + 12, rx = w - 12, tw = 70;
    _timeLbl.frame = CGRectMake(rx - tw, 14, tw, 16);
    _nameLbl.frame = CGRectMake(lx, 14, rx - tw - lx - 4, 22);
    CGSize bs = _badge.image ? _badge.image.size : CGSizeZero;
    _badge.frame = CGRectMake(rx - bs.width, h - 16 - bs.height, bs.width, bs.height);
    _lastLbl.frame = CGRectMake(lx, 39, rx - lx - (bs.width > 0 ? bs.width + 8 : 0), 18);
}
@end

@interface WAChatsViewController () <UISearchBarDelegate>
@property (nonatomic, strong) UITableView *table;
@property (nonatomic, strong) UILabel *empty;
@property (nonatomic, strong) UILabel *banner;
@property (nonatomic, strong) UISearchBar *searchBar;
@property (nonatomic, strong) NSMutableArray *chats;      // all chats, newest-first
@property (nonatomic, strong) NSMutableArray *filtered;   // search results
@property (nonatomic, copy) NSString *query;
@end

@implementation WAChatsViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = WALoc(@"chats_title");
    self.view.backgroundColor = [UIColor whiteColor];

    self.navigationItem.leftBarButtonItem =
        [[UIBarButtonItem alloc] initWithTitle:WALoc(@"settings") style:UIBarButtonItemStylePlain target:self action:@selector(openSettings)];
    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(newChat)];

    self.chats = [NSMutableArray array];
    self.filtered = [NSMutableArray array];

    self.table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStylePlain];
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.rowHeight = 70;
    self.table.separatorColor = [UIColor colorWithWhite:0.86 alpha:1.0];
    self.searchBar = [[UISearchBar alloc] initWithFrame:CGRectMake(0, 0, self.view.bounds.size.width, 44)];
    self.searchBar.placeholder = WALoc(@"search");
    self.searchBar.delegate = self;
    self.table.tableHeaderView = self.searchBar;
    [self.view addSubview:self.table];

    self.banner = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, self.view.bounds.size.width, 24)];
    self.banner.backgroundColor = [WATheme accent];
    self.banner.textColor = [UIColor whiteColor];
    self.banner.textAlignment = NSTextAlignmentCenter;
    self.banner.font = [UIFont systemFontOfSize:12];
    self.banner.text = WALoc(@"status_connecting");
    [self.view addSubview:self.banner];

    self.empty = [[UILabel alloc] init];
    self.empty.numberOfLines = 0;
    self.empty.textAlignment = NSTextAlignmentCenter;
    self.empty.textColor = [UIColor colorWithWhite:0.5 alpha:1.0];
    self.empty.font = [UIFont systemFontOfSize:15];
    self.empty.text = WALoc(@"no_chats");
    [self.view addSubview:self.empty];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onState:) name:WASessionStateChanged object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(reloadFromStore) name:WAChatStoreChanged object:nil];
    [self reloadFromStore];
    [self refreshBanner:[WASession shared].state];
}

// A display name for a chat: the history/contact name when we have one, else the number before the "@".
- (NSString *)displayNameFor:(NSString *)jid name:(NSString *)name
{
    if (name.length) return name;
    return [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
}

// The active rows: filtered by the search query, else all chats.
- (NSArray *)rows { return self.query.length ? self.filtered : self.chats; }

- (void)applyFilter
{
    [self.filtered removeAllObjects];
    if (self.query.length) {
        NSString *q = [self.query lowercaseString];
        for (NSDictionary *c in self.chats) {
            if ([[c[@"name"] lowercaseString] rangeOfString:q].location != NSNotFound ||
                [c[@"jid"] rangeOfString:q].location != NSNotFound) [self.filtered addObject:c];
        }
    }
    self.empty.hidden = [self rows].count > 0;
    [self.table reloadData];
}

// The chat list is whatever the persistent store holds (history-sync conversations + live messages), newest-first.
- (void)reloadFromStore
{
    [self.chats removeAllObjects];
    for (NSDictionary *row in [[WAChatStore shared] conversationsSorted]) {
        [self.chats addObject:@{ @"jid": row[@"jid"] ?: @"",
                                 @"name": [self displayNameFor:row[@"jid"] name:row[@"name"]],
                                 @"last": row[@"last"] ?: @"",
                                 @"ts": row[@"ts"] ?: @0,
                                 @"unread": row[@"unread"] ?: @0 }];
    }
    [self applyFilter];
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    CGFloat w = self.view.bounds.size.width, h = self.view.bounds.size.height;
    CGFloat bh = self.banner.hidden ? 0 : 24;
    self.banner.frame = CGRectMake(0, 0, w, bh);
    self.table.frame = CGRectMake(0, bh, w, h - bh);
    self.empty.frame = CGRectMake(30, 140, w - 60, 80);
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
    self.banner.hidden = (s == WASessionStateConnected);
    [self.view setNeedsLayout];
    self.empty.hidden = [self rows].count > 0;
}

- (void)onState:(NSNotification *)n { [self refreshBanner:[n.userInfo[@"state"] integerValue]]; }

#pragma mark - search

- (void)searchBar:(UISearchBar *)sb textDidChange:(NSString *)text { self.query = text; [self applyFilter]; }
- (void)searchBarSearchButtonClicked:(UISearchBar *)sb { [sb resignFirstResponder]; }
- (void)searchBarCancelButtonClicked:(UISearchBar *)sb { sb.text = @""; self.query = @""; [sb resignFirstResponder]; [self applyFilter]; }
- (void)searchBarTextDidBeginEditing:(UISearchBar *)sb { sb.showsCancelButton = YES; }
- (void)searchBarTextDidEndEditing:(UISearchBar *)sb { sb.showsCancelButton = NO; }

- (void)openSettings
{
    [self.navigationController pushViewController:[[WASettingsViewController alloc] init] animated:YES];
}

- (void)alertView:(UIAlertView *)alertView clickedButtonAtIndex:(NSInteger)buttonIndex
{
    if (alertView.tag == 2 && buttonIndex == 1) {
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

- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return [self rows].count; }

- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip
{
    WAChatCell *cell = (WAChatCell *)[t dequeueReusableCellWithIdentifier:@"c"];
    if (!cell) cell = [[WAChatCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"c"];
    NSDictionary *chat = [self rows][ip.row];
    NSString *name = chat[@"name"];
    cell.nameLbl.text = name;
    cell.lastLbl.text = chat[@"last"];
    cell.avatar.image = [WATheme avatarForName:name jid:chat[@"jid"] diameter:52];
    cell.timeLbl.text = WAListTime([chat[@"ts"] unsignedLongLongValue]);
    NSInteger unread = [chat[@"unread"] integerValue];
    cell.badge.image = unread > 0 ? [WATheme badgeWithCount:unread] : nil;
    [cell setNeedsLayout];
    return cell;
}

- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)ip
{
    [t deselectRowAtIndexPath:ip animated:YES];
    [self.searchBar resignFirstResponder];
    NSDictionary *chat = [self rows][ip.row];
    WAChatViewController *c = [[WAChatViewController alloc] initWithJID:chat[@"jid"] name:chat[@"name"]];
    [self.navigationController pushViewController:c animated:YES];
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

@end
