#import "WAContactInfoViewController.h"
#import "WACallViewController.h"
#import "WAMediaViewController.h"
#import "WAMediaGalleryViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"
#import "../WAChatStore.h"
#import <QuartzCore/QuartzCore.h>
#include "wa_history.h"

@interface WAContactInfoViewController () <UIActionSheetDelegate>
@property (nonatomic, copy) NSString *jid;
@property (nonatomic, copy) NSString *name;
@property (nonatomic) BOOL isGroup;
@property (nonatomic, strong) UITableView *table;
@property (nonatomic, strong) UIImageView *avatarView;
@property (nonatomic, strong) UILabel *nameLabel;
@property (nonatomic, strong) UILabel *phoneLabel;
@property (nonatomic, strong) NSArray *messages;
@property (nonatomic, strong) NSArray *participants;
@end

@implementation WAContactInfoViewController

- (instancetype)initWithJID:(NSString *)jid name:(NSString *)name
{
    if ((self = [super init])) {
        _jid = [jid copy];
        _name = [name copy];
        _isGroup = ([jid rangeOfString:@"@g.us"].location != NSNotFound);
    }
    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = self.isGroup ? WALoc(@"group_info") : WALoc(@"contact_info");
    self.view.backgroundColor = [UIColor colorWithWhite:0.94 alpha:1.0];

    self.table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStyleGrouped];
    self.table.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.table.dataSource = self;
    self.table.delegate = self;

    [self reloadChatData];
    [self setupHeader];

    [self.view addSubview:self.table];

    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemEdit target:self action:@selector(editNameTapped)];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onStoreChanged:) name:WAChatStoreChanged object:nil];
}

- (void)reloadChatData
{
    self.messages = [[WAChatStore shared] messagesForJID:self.jid];
    if (self.isGroup) {
        NSMutableSet *parts = [NSMutableSet set];
        for (NSDictionary *m in self.messages) {
            NSString *s = m[@"sender"];
            if (s.length) [parts addObject:s];
        }
        self.participants = [parts allObjects];
    }
}

- (void)onStoreChanged:(NSNotification *)n
{
    [self reloadChatData];
    [self.table reloadData];
}

- (void)setupHeader
{
    CGFloat w = self.view.bounds.size.width;
    UIView *hdr = [[UIView alloc] initWithFrame:CGRectMake(0, 0, w, 170)];
    hdr.autoresizingMask = UIViewAutoresizingFlexibleWidth;

    CGFloat avSize = 84;
    self.avatarView = [[UIImageView alloc] initWithFrame:CGRectMake((w - avSize) / 2, 16, avSize, avSize)];
    self.avatarView.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
    self.avatarView.layer.cornerRadius = avSize / 2;
    self.avatarView.clipsToBounds = YES;
    self.avatarView.userInteractionEnabled = YES;
    self.avatarView.image = [WATheme avatarForName:self.name jid:self.jid diameter:avSize];
    [self.avatarView addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(avatarTapped)]];
    [hdr addSubview:self.avatarView];

    self.nameLabel = [[UILabel alloc] initWithFrame:CGRectMake(20, 16 + avSize + 8, w - 40, 26)];
    self.nameLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    self.nameLabel.font = [UIFont boldSystemFontOfSize:20];
    self.nameLabel.textAlignment = NSTextAlignmentCenter;
    self.nameLabel.textColor = [UIColor blackColor];
    self.nameLabel.backgroundColor = [UIColor clearColor];
    self.nameLabel.text = WADisplayNameForChat(self.jid, self.name);
    [hdr addSubview:self.nameLabel];

    self.phoneLabel = [[UILabel alloc] initWithFrame:CGRectMake(20, 16 + avSize + 36, w - 40, 18)];
    self.phoneLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    self.phoneLabel.font = [UIFont systemFontOfSize:14];
    self.phoneLabel.textAlignment = NSTextAlignmentCenter;
    self.phoneLabel.textColor = [UIColor colorWithWhite:0.5 alpha:1.0];
    self.phoneLabel.backgroundColor = [UIColor clearColor];
    self.phoneLabel.text = self.isGroup ? [NSString stringWithFormat:@"%lu %@", (unsigned long)self.participants.count, WALoc(@"participants")] : WAFormatPhoneNumber(self.jid);
    [hdr addSubview:self.phoneLabel];

    self.table.tableHeaderView = hdr;
}

- (void)avatarTapped
{
    UIImage *img = [WATheme avatarForName:self.name jid:self.jid diameter:240];
    NSString *key = [WAChatStore keyForJID:self.jid];
    NSString *p = [WADocumentsPath() stringByAppendingPathComponent:[NSString stringWithFormat:@"wa6_avatar_%@.jpg", key]];
    if ([[NSFileManager defaultManager] fileExistsAtPath:p]) {
        UIImage *fileImg = [UIImage imageWithContentsOfFile:p];
        if (fileImg) img = fileImg;
    }
    WAMediaViewController *mv = [[WAMediaViewController alloc] initWithImage:img];
    mv.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
    [self presentViewController:mv animated:YES completion:nil];
}

#pragma mark - Table View

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView
{
    // 0: Calls, 1: Media/Info, 2: Preferences, 3: Participants or Block, 4: Clear/Delete
    return 5;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
    if (section == 0) return self.isGroup ? 0 : 2; // Call, Video Call
    if (section == 1) return 3; // Messages count, Media count, Starred count
    if (section == 2) return 3; // Mute, Wallpaper, Disappearing messages
    if (section == 3) return self.isGroup ? (self.participants.count ?: 1) : 1; // Participants list or Block contact
    if (section == 4) return self.isGroup ? 3 : 2; // (Leave Group), Clear Chat, Delete Chat
    return 0;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section
{
    if (section == 1) return WALoc(@"media_shared");
    if (section == 2) return WALoc(@"chat_settings");
    if (section == 3) return self.isGroup ? WALoc(@"participants") : nil;
    return nil;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
    NSString *cellId = [NSString stringWithFormat:@"Cell_%ld_%ld", (long)indexPath.section, (long)indexPath.row];
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:cellId];
    if (!cell) {
        UITableViewCellStyle style = (indexPath.section == 1 || indexPath.section == 2) ? UITableViewCellStyleValue1 : UITableViewCellStyleDefault;
        cell = [[UITableViewCell alloc] initWithStyle:style reuseIdentifier:cellId];
    }
    cell.imageView.image = nil;

    if (indexPath.section == 0) {
        if (indexPath.row == 0) {
            cell.textLabel.text = WALoc(@"call_voice");
            cell.textLabel.textColor = [WATheme accent];
            cell.imageView.image = [WATheme callIconWithColor:[WATheme accent] size:CGSizeMake(20, 20)];
        } else {
            cell.textLabel.text = WALoc(@"call_video");
            cell.textLabel.textColor = [WATheme accent];
            cell.imageView.image = [WATheme videoCallIconWithColor:[WATheme accent] size:CGSizeMake(20, 20)];
        }
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    } else if (indexPath.section == 1) {
        if (indexPath.row == 0) {
            cell.textLabel.text = WALoc(@"message_hint");
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%lu %@", (unsigned long)self.messages.count, WALoc(@"messages_count")];
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            cell.accessoryType = UITableViewCellAccessoryNone;
        } else if (indexPath.row == 1) {
            NSUInteger mediaCount = 0;
            for (NSDictionary *m in self.messages) {
                int k = [m[@"kind"] intValue];
                if (k == WA_MSG_IMAGE || k == WA_MSG_VIDEO) mediaCount++;
            }
            cell.textLabel.text = WALoc(@"media_shared");
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%lu %@", (unsigned long)mediaCount, WALoc(@"media_count")];
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.selectionStyle = UITableViewCellSelectionStyleBlue;
        } else {
            NSUInteger starCount = [[WAChatStore shared] starredMessagesForJID:self.jid].count;
            cell.textLabel.text = WALoc(@"starred_messages");
            cell.imageView.image = [WATheme starIconWithColor:[UIColor colorWithRed:0.95 green:0.75 blue:0.1 alpha:1.0] size:CGSizeMake(18, 18)];
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%lu", (unsigned long)starCount];
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            cell.accessoryType = UITableViewCellAccessoryNone;
        }
    } else if (indexPath.section == 2) {
        if (indexPath.row == 0) {
            cell.textLabel.text = WALoc(@"mute_notifications");
            BOOL isMuted = [[WAChatStore shared] isChatMuted:self.jid];
            cell.detailTextLabel.text = isMuted ? WALoc(@"call_mute") : WALoc(@"disappearing_off");
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        } else if (indexPath.row == 1) {
            cell.textLabel.text = WALoc(@"wallpaper");
            WAWallpaperType wt = [WATheme currentWallpaperType];
            NSString *wname = WALoc(@"wallpaper_classic");
            if (wt == WAWallpaperTypeParchment) wname = WALoc(@"wallpaper_parchment");
            else if (wt == WAWallpaperTypeSlate) wname = WALoc(@"wallpaper_slate");
            else if (wt == WAWallpaperTypeSky) wname = WALoc(@"wallpaper_sky");
            cell.detailTextLabel.text = wname;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        } else {
            cell.textLabel.text = WALoc(@"disappearing_messages");
            NSTimeInterval dur = [[WAChatStore shared] ephemeralDurationForChat:self.jid];
            NSString *durStr = WALoc(@"disappearing_off");
            if (dur == 86400) durStr = WALoc(@"disappearing_24h");
            else if (dur == 7 * 86400) durStr = WALoc(@"disappearing_7d");
            else if (dur == 90 * 86400) durStr = WALoc(@"disappearing_90d");
            cell.detailTextLabel.text = durStr;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        }
    } else if (indexPath.section == 3) {
        if (self.isGroup) {
            if (self.participants.count > 0) {
                NSString *p = self.participants[indexPath.row];
                cell.textLabel.text = WADisplayNameForChat(p, nil);
                cell.imageView.image = [WATheme avatarForName:cell.textLabel.text jid:p diameter:36];
            } else {
                cell.textLabel.text = WALoc(@"group_default");
                cell.imageView.image = nil;
            }
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            cell.accessoryType = UITableViewCellAccessoryNone;
        } else {
            BOOL isBlocked = [[WAChatStore shared] isContactBlocked:self.jid];
            cell.textLabel.text = isBlocked ? WALoc(@"unblock_contact") : WALoc(@"block_contact");
            cell.textLabel.textColor = isBlocked ? [UIColor colorWithRed:0.2 green:0.5 blue:0.2 alpha:1.0] : [UIColor colorWithRed:0.85 green:0.2 blue:0.2 alpha:1.0];
            cell.textLabel.textAlignment = NSTextAlignmentCenter;
            cell.accessoryType = UITableViewCellAccessoryNone;
        }
    } else if (indexPath.section == 4) {
        if (self.isGroup && indexPath.row == 0) {
            cell.textLabel.text = WALoc(@"leave_group");
            cell.textLabel.textColor = [UIColor colorWithRed:0.85 green:0.2 blue:0.2 alpha:1.0];
        } else {
            NSInteger idx = self.isGroup ? (indexPath.row - 1) : indexPath.row;
            if (idx == 0) {
                cell.textLabel.text = WALoc(@"clear_chat");
                cell.textLabel.textColor = [UIColor colorWithRed:0.85 green:0.2 blue:0.2 alpha:1.0];
            } else {
                cell.textLabel.text = WALoc(@"delete_chat");
                cell.textLabel.textColor = [UIColor colorWithRed:0.85 green:0.2 blue:0.2 alpha:1.0];
            }
        }
        cell.textLabel.textAlignment = NSTextAlignmentCenter;
        cell.accessoryType = UITableViewCellAccessoryNone;
    }

    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
    [tableView deselectRowAtIndexPath:indexPath animated:YES];

    if (indexPath.section == 0) {
        BOOL isVideo = (indexPath.row == 1);
        [[WASession shared] placeCallTo:self.jid video:isVideo];
        WACallViewController *c = [[WACallViewController alloc] initWithJID:self.jid name:self.name video:isVideo incoming:NO];
        c.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
        [self presentViewController:c animated:YES completion:nil];
    } else if (indexPath.section == 1) {
        if (indexPath.row == 1) { // Media Gallery
            WAMediaGalleryViewController *gallery = [[WAMediaGalleryViewController alloc] initWithMessages:self.messages];
            UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:gallery];
            [WATheme styleNavigationBar:nav.navigationBar];
            [self presentViewController:nav animated:YES completion:nil];
        }
    } else if (indexPath.section == 2) {
        if (indexPath.row == 0) { // Mute notifications
            BOOL isMuted = [[WAChatStore shared] isChatMuted:self.jid];
            if (isMuted) {
                [[WAChatStore shared] muteChat:self.jid duration:0];
                [self.table reloadData];
            } else {
                UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"mute_notifications")
                                                                delegate:self
                                                       cancelButtonTitle:WALoc(@"cancel")
                                                  destructiveButtonTitle:nil
                                                       otherButtonTitles:WALoc(@"mute_8hours"), WALoc(@"mute_1week"), WALoc(@"mute_always"), nil];
                as.tag = 101;
                [as showInView:self.view];
            }
        } else if (indexPath.row == 1) { // Wallpaper
            UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"wallpaper")
                                                            delegate:self
                                                   cancelButtonTitle:WALoc(@"cancel")
                                              destructiveButtonTitle:nil
                                                   otherButtonTitles:WALoc(@"wallpaper_classic"), WALoc(@"wallpaper_parchment"), WALoc(@"wallpaper_slate"), WALoc(@"wallpaper_sky"), nil];
            as.tag = 102;
            [as showInView:self.view];
        } else if (indexPath.row == 2) { // Disappearing messages
            UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"disappearing_messages")
                                                            delegate:self
                                                   cancelButtonTitle:WALoc(@"cancel")
                                              destructiveButtonTitle:nil
                                                   otherButtonTitles:WALoc(@"disappearing_off"), WALoc(@"disappearing_24h"), WALoc(@"disappearing_7d"), WALoc(@"disappearing_90d"), nil];
            as.tag = 103;
            [as showInView:self.view];
        }
    } else if (indexPath.section == 3) {
        if (!self.isGroup) {
            BOOL isBlocked = [[WAChatStore shared] isContactBlocked:self.jid];
            BOOL nextState = !isBlocked;
            [[WAChatStore shared] setContact:self.jid blocked:nextState];
            [[WASession shared] setBlockContact:self.jid blocked:nextState];
            [self.table reloadData];
            UIAlertView *av = [[UIAlertView alloc] initWithTitle:nil message:(nextState ? WALoc(@"contact_blocked") : WALoc(@"unblock_contact")) delegate:nil cancelButtonTitle:WALoc(@"ok") otherButtonTitles:nil];
            [av show];
        }
    } else if (indexPath.section == 4) {
        if (self.isGroup && indexPath.row == 0) { // Leave Group
            UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"leave_group_confirm")
                                                            delegate:self
                                                   cancelButtonTitle:WALoc(@"cancel")
                                              destructiveButtonTitle:WALoc(@"leave_group")
                                                   otherButtonTitles:nil];
            as.tag = 104;
            [as showInView:self.view];
            return;
        }
        NSInteger idx = self.isGroup ? (indexPath.row - 1) : indexPath.row;
        if (idx == 0) { // Clear
            UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"clear_chat_confirm")
                                                            delegate:self
                                                   cancelButtonTitle:WALoc(@"cancel")
                                              destructiveButtonTitle:WALoc(@"clear")
                                                   otherButtonTitles:nil];
            as.tag = 105;
            [as showInView:self.view];
        } else { // Delete
            UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"delete_chat_confirm")
                                                            delegate:self
                                                   cancelButtonTitle:WALoc(@"cancel")
                                              destructiveButtonTitle:WALoc(@"delete")
                                                   otherButtonTitles:nil];
            as.tag = 106;
            [as showInView:self.view];
        }
    }
}

- (void)actionSheet:(UIActionSheet *)actionSheet clickedButtonAtIndex:(NSInteger)buttonIndex
{
    if (actionSheet.tag == 101) { // Mute duration
        if (buttonIndex == 0) [[WAChatStore shared] muteChat:self.jid duration:8 * 3600];
        else if (buttonIndex == 1) [[WAChatStore shared] muteChat:self.jid duration:7 * 86400];
        else if (buttonIndex == 2) [[WAChatStore shared] muteChat:self.jid duration:2000000000];
        [self.table reloadData];
    } else if (actionSheet.tag == 102) { // Wallpaper
        if (buttonIndex >= 0 && buttonIndex <= 3) {
            [WATheme setWallpaperType:(WAWallpaperType)buttonIndex];
            [self.table reloadData];
        }
    } else if (actionSheet.tag == 103) { // Disappearing messages
        NSTimeInterval dur = 0;
        if (buttonIndex == 1) dur = 86400;
        else if (buttonIndex == 2) dur = 7 * 86400;
        else if (buttonIndex == 3) dur = 90 * 86400;
        [[WAChatStore shared] setEphemeralDuration:dur forChat:self.jid];
        [self.table reloadData];
    } else if (actionSheet.tag == 104) { // Leave group
        if (buttonIndex == actionSheet.destructiveButtonIndex) {
            [[WAChatStore shared] deleteChat:self.jid];
            [self.navigationController popToRootViewControllerAnimated:YES];
        }
    } else if (actionSheet.tag == 105) { // Clear
        if (buttonIndex == actionSheet.destructiveButtonIndex) {
            [[WAChatStore shared] clearChat:self.jid];
            [self reloadChatData];
            [self.table reloadData];
        }
    } else if (actionSheet.tag == 106) { // Delete
        if (buttonIndex == actionSheet.destructiveButtonIndex) {
            [[WAChatStore shared] deleteChat:self.jid];
            [self.navigationController popToRootViewControllerAnimated:YES];
        }
    }
}

- (void)editNameTapped
{
    UIAlertView *av = [[UIAlertView alloc] initWithTitle:WALoc(@"edit_name")
                                                message:WALoc(@"edit_name_prompt")
                                               delegate:self
                                      cancelButtonTitle:WALoc(@"cancel")
                                      otherButtonTitles:WALoc(@"save"), nil];
    av.alertViewStyle = UIAlertViewStylePlainTextInput;
    UITextField *tf = [av textFieldAtIndex:0];
    tf.text = WADisplayNameForChat(self.jid, self.name);
    av.tag = 992;
    [av show];
}

- (void)alertView:(UIAlertView *)alertView clickedButtonAtIndex:(NSInteger)buttonIndex
{
    if (alertView.tag == 992 && buttonIndex == 1) {
        UITextField *tf = [alertView textFieldAtIndex:0];
        NSString *newName = [tf.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        [[WAChatStore shared] setCustomName:newName forJID:self.jid];
        self.name = newName;
        self.nameLabel.text = WADisplayNameForChat(self.jid, newName);
        self.avatarView.image = [WATheme avatarForName:newName jid:self.jid diameter:84];
        [self.table reloadData];
    }
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

@end
