#import "WAChatsViewController.h"
#import "WAChatViewController.h"
#import "WASettingsViewController.h"
#import "WACallViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"
#import "../WAChatStore.h"
#import "../WAAppDelegate.h"
#import <AddressBookUI/AddressBookUI.h>

// A WhatsApp-style chat-list row: avatar, name, last-message preview, time, unread badge, pin & mute indicators.
@interface WAChatCell : UITableViewCell
@property (nonatomic, strong) UIImageView *avatar;
@property (nonatomic, strong) UILabel *nameLbl;
@property (nonatomic, strong) UILabel *lastLbl;
@property (nonatomic, strong) UILabel *timeLbl;
@property (nonatomic, strong) UIImageView *badge;
@property (nonatomic, strong) UIImageView *pinView;
@property (nonatomic, strong) UIImageView *muteView;
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

        _pinView = [[UIImageView alloc] init];
        _pinView.image = [WATheme pinIconWithColor:[UIColor colorWithRed:0.78 green:0.25 blue:0.2 alpha:1.0] size:CGSizeMake(14, 14)];
        _pinView.contentMode = UIViewContentModeCenter;
        _pinView.hidden = YES;

        _muteView = [[UIImageView alloc] init];
        _muteView.image = [WATheme muteIconWithColor:[UIColor colorWithWhite:0.55 alpha:1.0] size:CGSizeMake(15, 14)];
        _muteView.contentMode = UIViewContentModeCenter;
        _muteView.hidden = YES;

        for (UIView *v in @[ _avatar, _nameLbl, _lastLbl, _timeLbl, _badge, _pinView, _muteView ]) [self.contentView addSubview:v];
    }
    return self;
}
- (void)layoutSubviews
{
    [super layoutSubviews];
    CGFloat w = self.contentView.bounds.size.width, h = self.contentView.bounds.size.height, av = 52;
    _avatar.frame = CGRectMake(10, (h - av) / 2, av, av);
    CGFloat lx = 10 + av + 12, rx = w - 12, tw = 70;
    CGFloat pinW = _pinView.hidden ? 0 : 16;
    CGFloat muteW = _muteView.hidden ? 0 : 18;

    _timeLbl.frame = CGRectMake(rx - tw, 14, tw, 16);
    _nameLbl.frame = CGRectMake(lx, 14, rx - tw - lx - 4 - pinW, 22);
    if (!_pinView.hidden) {
        _pinView.frame = CGRectMake(rx - tw - pinW, 16, pinW, 16);
    }
    CGSize bs = _badge.image ? _badge.image.size : CGSizeZero;
    _badge.frame = CGRectMake(rx - bs.width, h - 16 - bs.height, bs.width, bs.height);
    CGFloat rightIcons = (bs.width > 0 ? bs.width + 6 : 0) + (muteW > 0 ? muteW + 6 : 0);
    _lastLbl.frame = CGRectMake(lx, 39, rx - lx - rightIcons, 18);
    if (!_muteView.hidden) {
        _muteView.frame = CGRectMake(rx - (bs.width > 0 ? bs.width + 6 : 0) - muteW, 40, muteW, 16);
    }
}
@end

@interface WAChatsViewController () <UISearchBarDelegate, UIActionSheetDelegate, ABPeoplePickerNavigationControllerDelegate>
@property (nonatomic, strong) UITableView *table;
@property (nonatomic, strong) UILabel *empty;
@property (nonatomic, strong) UILabel *banner;
@property (nonatomic, strong) UISearchBar *searchBar;
@property (nonatomic, strong) NSMutableArray *chats;      // all chats, newest-first
@property (nonatomic, strong) NSMutableArray *filtered;   // search results
@property (nonatomic, strong) NSMutableArray *callLogs;   // call history
@property (nonatomic, copy) NSString *query;
@property (nonatomic, strong) NSMutableSet *requestedAvatars;
@property (nonatomic, strong) NSDictionary *selectedChatForAction;
@end

@implementation WAChatsViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor whiteColor];

    self.navSegment = [[UISegmentedControl alloc] initWithItems:@[ WALoc(@"chats_title"), WALoc(@"calls_tab") ]];
    self.navSegment.segmentedControlStyle = UISegmentedControlStyleBar;
    self.navSegment.selectedSegmentIndex = 0;
    [self.navSegment addTarget:self action:@selector(onSegmentChanged) forControlEvents:UIControlEventValueChanged];
    self.navigationItem.titleView = self.navSegment;

    self.navigationItem.leftBarButtonItem =
        [[UIBarButtonItem alloc] initWithTitle:WALoc(@"settings") style:UIBarButtonItemStylePlain target:self action:@selector(openSettings)];
    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(newChat)];

    self.chats = [NSMutableArray array];
    self.filtered = [NSMutableArray array];
    self.callLogs = [NSMutableArray array];

    self.table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStylePlain];
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.rowHeight = 70;
    self.table.separatorColor = [UIColor colorWithWhite:0.86 alpha:1.0];
    [self.table addGestureRecognizer:[[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(onTableLongPress:)]];

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

- (void)onSegmentChanged
{
    if (self.navSegment.selectedSegmentIndex == 0) {
        self.navigationItem.rightBarButtonItem =
            [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(newChat)];
        self.table.tableHeaderView = self.searchBar;
        self.empty.text = WALoc(@"no_chats");
    } else {
        self.navigationItem.rightBarButtonItem =
            [[UIBarButtonItem alloc] initWithTitle:WALoc(@"clear") style:UIBarButtonItemStylePlain target:self action:@selector(clearCalls)];
        self.table.tableHeaderView = nil;
        self.empty.text = WALoc(@"call_history");
    }
    [self updateEmptyState];
    [self.table reloadData];
}

- (void)updateEmptyState
{
    if (self.navSegment.selectedSegmentIndex == 0) {
        self.empty.hidden = [self rows].count > 0;
    } else {
        self.empty.hidden = self.callLogs.count > 0;
    }
}

- (NSString *)displayNameFor:(NSString *)jid name:(NSString *)name
{
    return WADisplayNameForChat(jid, name);
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
                [c[@"jid"] rangeOfString:q].location != NSNotFound ||
                [[c[@"last"] lowercaseString] rangeOfString:q].location != NSNotFound) [self.filtered addObject:c];
        }
    }
    [self updateEmptyState];
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
                                 @"unread": row[@"unread"] ?: @0,
                                 @"pinned": row[@"pinned"] ?: @NO,
                                 @"muted": row[@"muted"] ?: @NO }];
    }
    self.callLogs = [[[WAChatStore shared] callLogs] mutableCopy];
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
    [self updateEmptyState];
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

- (void)clearCalls
{
    UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"clear_call_history")
                                                    delegate:self
                                           cancelButtonTitle:WALoc(@"cancel")
                                      destructiveButtonTitle:WALoc(@"clear")
                                           otherButtonTitles:nil];
    as.tag = 600;
    if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
        [as showFromBarButtonItem:self.navigationItem.rightBarButtonItem animated:YES];
    } else {
        [as showInView:self.view];
    }
}

#pragma mark - Long press contextual actions

- (void)onTableLongPress:(UILongPressGestureRecognizer *)g
{
    if (self.navSegment.selectedSegmentIndex != 0) return;
    if (g.state != UIGestureRecognizerStateBegan) return;
    CGPoint p = [g locationInView:self.table];
    NSIndexPath *ip = [self.table indexPathForRowAtPoint:p];
    if (!ip || ip.row >= [self rows].count) return;

    self.selectedChatForAction = [self rows][ip.row];
    NSString *jid = self.selectedChatForAction[@"jid"];
    BOOL isPinned = [[WAChatStore shared] isChatPinned:jid];
    BOOL isMuted = [[WAChatStore shared] isChatMuted:jid];

    UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:self.selectedChatForAction[@"name"]
                                                    delegate:self
                                           cancelButtonTitle:WALoc(@"cancel")
                                      destructiveButtonTitle:WALoc(@"delete_chat")
                                           otherButtonTitles:(isPinned ? WALoc(@"unpin_chat") : WALoc(@"pin_chat")),
                                                             (isMuted ? WALoc(@"unmute_notifications") : WALoc(@"mute_notifications")),
                                                             WALoc(@"clear_chat"), nil];
    as.tag = 500;
    if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
        UITableViewCell *cell = [self.table cellForRowAtIndexPath:ip];
        [as showFromRect:(cell ? cell.bounds : CGRectMake(p.x, p.y, 1, 1)) inView:(cell ?: self.view) animated:YES];
    } else {
        [as showInView:self.view];
    }
}

- (void)actionSheet:(UIActionSheet *)actionSheet clickedButtonAtIndex:(NSInteger)buttonIndex
{
    if (actionSheet.tag == 500) {
        if (!self.selectedChatForAction) return;
        NSString *jid = self.selectedChatForAction[@"jid"];
        if (buttonIndex == actionSheet.destructiveButtonIndex) {
            [[WAChatStore shared] deleteChat:jid];
        } else if (buttonIndex == 1) { // Pin / Unpin
            BOOL isPinned = [[WAChatStore shared] isChatPinned:jid];
            [[WAChatStore shared] setChat:jid pinned:!isPinned];
        } else if (buttonIndex == 2) { // Mute / Unmute
            BOOL isMuted = [[WAChatStore shared] isChatMuted:jid];
            if (isMuted) {
                [[WAChatStore shared] muteChat:jid duration:0];
            } else {
                UIActionSheet *muteSheet = [[UIActionSheet alloc] initWithTitle:WALoc(@"mute_notifications")
                                                                       delegate:self
                                                              cancelButtonTitle:WALoc(@"cancel")
                                                         destructiveButtonTitle:nil
                                                              otherButtonTitles:WALoc(@"mute_8hours"), WALoc(@"mute_1week"), WALoc(@"mute_always"), nil];
                muteSheet.tag = 501;
                if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
                    [muteSheet showFromRect:self.table.bounds inView:self.table animated:YES];
                } else {
                    [muteSheet showInView:self.view];
                }
            }
        } else if (buttonIndex == 3) { // Clear
            [[WAChatStore shared] clearChat:jid];
        }
    } else if (actionSheet.tag == 501) {
        if (!self.selectedChatForAction) return;
        NSString *jid = self.selectedChatForAction[@"jid"];
        if (buttonIndex == 0) [[WAChatStore shared] muteChat:jid duration:8 * 3600];
        else if (buttonIndex == 1) [[WAChatStore shared] muteChat:jid duration:7 * 86400];
        else if (buttonIndex == 2) [[WAChatStore shared] muteChat:jid duration:2000000000];
    } else if (actionSheet.tag == 600) {
        if (buttonIndex == actionSheet.destructiveButtonIndex) {
            [[WAChatStore shared] clearCallLogs];
            [self.callLogs removeAllObjects];
            [self.table reloadData];
            [self updateEmptyState];
        }
    } else if (actionSheet.tag == 100) {
        if (buttonIndex == 0) {
            ABPeoplePickerNavigationController *picker = [[ABPeoplePickerNavigationController alloc] init];
            picker.peoplePickerDelegate = self;
            [self presentViewController:picker animated:YES completion:nil];
        } else if (buttonIndex == 1) {
            UIAlertView *alert = [[UIAlertView alloc] initWithTitle:WALoc(@"enter_number")
                                                            message:nil
                                                           delegate:self
                                                  cancelButtonTitle:WALoc(@"cancel")
                                                  otherButtonTitles:WALoc(@"ok"), nil];
            alert.alertViewStyle = UIAlertViewStylePlainTextInput;
            alert.tag = 2;
            UITextField *tf = [alert textFieldAtIndex:0];
            tf.keyboardType = UIKeyboardTypePhonePad;
            tf.placeholder = @"+420...";
            [alert show];
        }
    }
}

#pragma mark - new chat

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
    UIActionSheet *sheet = [[UIActionSheet alloc] initWithTitle:nil
                                                       delegate:self
                                              cancelButtonTitle:WALoc(@"cancel")
                                         destructiveButtonTitle:nil
                                              otherButtonTitles:WALoc(@"choose_contact"), WALoc(@"enter_number"), nil];
    sheet.tag = 100;
    if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
        [sheet showFromBarButtonItem:self.navigationItem.rightBarButtonItem animated:YES];
    } else {
        [sheet showInView:self.view];
    }
}

- (BOOL)peoplePickerNavigationController:(ABPeoplePickerNavigationController *)peoplePicker shouldContinueAfterSelectingPerson:(ABRecordRef)person
{
    ABMultiValueRef phones = ABRecordCopyValue(person, kABPersonPhoneProperty);
    if (phones) {
        CFIndex count = ABMultiValueGetCount(phones);
        if (count > 0) {
            CFIndex idx = -1;
            for (CFIndex i = 0; i < count; i++) {
                CFStringRef lbl = ABMultiValueCopyLabelAtIndex(phones, i);
                if (lbl) {
                    if (CFStringCompare(lbl, kABPersonPhoneMobileLabel, 0) == kCFCompareEqualTo ||
                        CFStringCompare(lbl, kABPersonPhoneIPhoneLabel, 0) == kCFCompareEqualTo ||
                        CFStringCompare(lbl, kABPersonPhoneMainLabel, 0) == kCFCompareEqualTo) {
                        idx = i;
                        CFRelease(lbl);
                        break;
                    }
                    CFRelease(lbl);
                }
            }
            if (idx == -1) idx = 0;
            if (idx != -1) {
                CFStringRef ph = ABMultiValueCopyValueAtIndex(phones, idx);
                NSString *phoneStr = [NSString stringWithString:(__bridge NSString *)ph];
                CFRelease(ph);
                CFStringRef fn = ABRecordCopyCompositeName(person);
                NSString *fullName = fn ? [NSString stringWithString:(__bridge NSString *)fn] : nil;
                if (fn) CFRelease(fn);
                CFRelease(phones);
                [self dismissViewControllerAnimated:YES completion:^{
                    [self openChatWithNumber:phoneStr name:fullName];
                }];
                return NO;
            }
            CFRelease(phones);
        }
    }
    return NO;
}

- (BOOL)peoplePickerNavigationController:(ABPeoplePickerNavigationController *)peoplePicker shouldContinueAfterSelectingPerson:(ABRecordRef)person property:(ABPropertyID)property identifier:(ABMultiValueIdentifier)identifier
{
    return NO;
}

- (void)peoplePickerNavigationControllerDidCancel:(ABPeoplePickerNavigationController *)peoplePicker
{
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)openChatWithNumber:(NSString *)num
{
    [self openChatWithNumber:num name:nil];
}

- (void)openChatWithNumber:(NSString *)num name:(NSString *)name
{
    NSString *digits = [[num componentsSeparatedByCharactersInSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]] componentsJoinedByString:@""];
    if (!digits.length) return;
    NSString *jid = [digits stringByAppendingString:@"@s.whatsapp.net"];
    if (!name.length) name = WADisplayNameForChat(jid, nil);
    WAChatViewController *c = [[WAChatViewController alloc] initWithJID:jid name:name];
    [self.navigationController pushViewController:c animated:YES];
}

#pragma mark - table

- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s
{
    if (self.navSegment.selectedSegmentIndex == 1) return self.callLogs.count;
    return [self rows].count;
}

- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip
{
    if (self.navSegment.selectedSegmentIndex == 1) {
        UITableViewCell *cell = [t dequeueReusableCellWithIdentifier:@"call_cell"];
        if (!cell) {
            cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"call_cell"];
            UIButton *callBtn = [UIButton buttonWithType:UIButtonTypeCustom];
            [callBtn setImage:[WATheme roundCallButton:[WATheme accent] diameter:32 glyph:@"☎"] forState:UIControlStateNormal];
            callBtn.frame = CGRectMake(0, 0, 32, 32);
            callBtn.tag = 1001;
            cell.accessoryView = callBtn;
        }
        NSDictionary *log = self.callLogs[ip.row];
        NSString *cname = log[@"name"] ?: WADisplayNameForChat(log[@"jid"], nil);
        cell.textLabel.text = cname;
        cell.textLabel.font = [UIFont boldSystemFontOfSize:16];
        cell.imageView.image = [WATheme avatarForName:cname jid:log[@"jid"] diameter:44];

        BOOL missed = [log[@"missed"] boolValue];
        BOOL incoming = [log[@"incoming"] boolValue];
        NSString *dirText = missed ? [NSString stringWithFormat:@"↙ %@", WALoc(@"missed_call")] :
                            (incoming ? [NSString stringWithFormat:@"↙ %@", WALoc(@"incoming_call_item")] :
                                        [NSString stringWithFormat:@"↗ %@", WALoc(@"outgoing_call_item")]);
        NSString *tstr = WAListTime([log[@"ts"] unsignedLongLongValue]);
        cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ • %@", dirText, tstr];
        cell.detailTextLabel.textColor = missed ? [UIColor colorWithRed:0.85 green:0.2 blue:0.2 alpha:1.0] : [UIColor colorWithWhite:0.5 alpha:1.0];

        UIButton *btn = (UIButton *)cell.accessoryView;
        btn.tag = ip.row;
        [btn removeTarget:nil action:NULL forControlEvents:UIControlEventAllEvents];
        [btn addTarget:self action:@selector(callLogButtonTapped:) forControlEvents:UIControlEventTouchUpInside];
        return cell;
    }

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

    cell.pinView.hidden = ![chat[@"pinned"] boolValue];
    cell.muteView.hidden = ![chat[@"muted"] boolValue];

    NSString *jid = chat[@"jid"];
    if (jid.length) {
        NSString *key = [WAChatStore keyForJID:jid];
        NSString *p = [WADocumentsPath() stringByAppendingPathComponent:[NSString stringWithFormat:@"wa6_avatar_%@.jpg", key]];
        if (![[NSFileManager defaultManager] fileExistsAtPath:p]) {
            if (!self.requestedAvatars) self.requestedAvatars = [NSMutableSet set];
            if (![self.requestedAvatars containsObject:key]) {
                [self.requestedAvatars addObject:key];
                [[WASession shared] fetchProfilePicture:jid];
            }
        }
    }

    [cell setNeedsLayout];
    return cell;
}

- (void)callLogButtonTapped:(UIButton *)sender
{
    NSInteger idx = sender.tag;
    if (idx < self.callLogs.count) {
        NSDictionary *log = self.callLogs[idx];
        NSString *jid = log[@"jid"];
        NSString *name = log[@"name"];
        BOOL video = [log[@"video"] boolValue];
        [[WASession shared] placeCallTo:jid video:video];
        WACallViewController *c = [[WACallViewController alloc] initWithJID:jid name:name video:video incoming:NO];
        c.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
        [self presentViewController:c animated:YES completion:nil];
    }
}

- (BOOL)tableView:(UITableView *)t canEditRowAtIndexPath:(NSIndexPath *)ip
{
    return (self.navSegment.selectedSegmentIndex == 0);
}

- (void)tableView:(UITableView *)t commitEditingStyle:(UITableViewCellEditingStyle)style forRowAtIndexPath:(NSIndexPath *)ip
{
    if (style != UITableViewCellEditingStyleDelete) return;
    NSString *jid = [self rows][ip.row][@"jid"];
    [[WAChatStore shared] deleteChat:jid];   // this posts WAChatStoreChanged -> reloadFromStore refreshes the table
}

- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)ip
{
    [t deselectRowAtIndexPath:ip animated:YES];
    [self.searchBar resignFirstResponder];
    if (self.navSegment.selectedSegmentIndex == 1) {
        NSDictionary *log = self.callLogs[ip.row];
        NSString *jid = log[@"jid"];
        NSString *name = log[@"name"];
        BOOL video = [log[@"video"] boolValue];
        [[WASession shared] placeCallTo:jid video:video];
        WACallViewController *c = [[WACallViewController alloc] initWithJID:jid name:name video:video incoming:NO];
        c.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
        [self presentViewController:c animated:YES completion:nil];
        return;
    }
    NSDictionary *chat = [self rows][ip.row];
    WAChatViewController *c = [[WAChatViewController alloc] initWithJID:chat[@"jid"] name:chat[@"name"]];
    [self.navigationController pushViewController:c animated:YES];
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

@end
