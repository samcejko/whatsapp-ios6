#import "WASettingsViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"
#import "../WAChatStore.h"
#import "../WAAppDelegate.h"
#import "../WADeviceStore.h"
#import <AudioToolbox/AudioToolbox.h>

@interface WASettingsViewController () <UIActionSheetDelegate>
@property (nonatomic, strong) UIImageView *profileAvatarView;
@property (nonatomic, strong) UILabel *statusSub;
@property (nonatomic, strong) NSArray *blockedList;
@end

@implementation WASettingsViewController {
    UITableView *_table;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = WALoc(@"settings");
    self.view.backgroundColor = [WATheme chatBackgroundColor];

    _table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStyleGrouped];
    _table.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _table.dataSource = self;
    _table.delegate = self;
    _table.backgroundColor = [UIColor clearColor];
    _table.separatorColor = [WATheme secondaryActionColor];

    // profile header: avatar + own number
    NSString *jid = [WASession shared].ownJID ?: @"";
    NSString *user = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
    NSString *num = [user componentsSeparatedByString:@":"].firstObject;
    NSString *bareJid = num.length ? [num stringByAppendingString:@"@s.whatsapp.net"] : jid;
    if (bareJid.length) {
        NSString *key = [WAChatStore keyForJID:bareJid];
        NSString *p = [WADocumentsPath() stringByAppendingPathComponent:[NSString stringWithFormat:@"wa6_avatar_%@.jpg", key]];
        if (![[NSFileManager defaultManager] fileExistsAtPath:p]) {
            [[WASession shared] fetchProfilePicture:bareJid];
        }
    }
    CGFloat w = self.view.bounds.size.width;
    UIView *hdr = [[UIView alloc] initWithFrame:CGRectMake(0, 0, w, 108)];
    self.profileAvatarView = [[UIImageView alloc] initWithFrame:CGRectMake(18, 22, 64, 64)];
    self.profileAvatarView.image = [WATheme avatarForName:WALoc(@"app_name") jid:(bareJid.length ? bareJid : @"wazzup") diameter:64];
    [hdr addSubview:self.profileAvatarView];
    UILabel *nl = [[UILabel alloc] initWithFrame:CGRectMake(96, 36, w - 110, 26)];
    nl.font = [WATheme headlineFont];
    nl.text = num.length ? [@"+" stringByAppendingString:num] : WALoc(@"profile");
    nl.backgroundColor = [UIColor clearColor];
    [hdr addSubview:nl];
    self.statusSub = [[UILabel alloc] initWithFrame:CGRectMake(96, 64, w - 110, 20)];
    self.statusSub.font = [WATheme captionFont];
    self.statusSub.textColor = [WATheme secondaryActionColor];
    self.statusSub.backgroundColor = [UIColor clearColor];
    [hdr addSubview:self.statusSub];
    _table.tableHeaderView = hdr;

    [self.view addSubview:_table];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(updateStatusText) name:WASessionStateChanged object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onStoreChanged:) name:WAChatStoreChanged object:nil];
    [self updateStatusText];
}

- (void)onStoreChanged:(NSNotification *)n
{
    NSString *jid = [WASession shared].ownJID ?: @"";
    NSString *user = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
    NSString *num = [user componentsSeparatedByString:@":"].firstObject;
    NSString *bareJid = num.length ? [num stringByAppendingString:@"@s.whatsapp.net"] : @"wazzup";
    self.profileAvatarView.image = [WATheme avatarForName:WALoc(@"app_name") jid:bareJid diameter:64];
}

- (void)updateStatusText
{
    WASessionState s = [WASession shared].state;
    NSString *t = WALoc(@"status_offline");
    if (s == WASessionStateConnecting) t = WALoc(@"status_connecting");
    else if (s == WASessionStateHandshaking) t = WALoc(@"status_handshaking");
    else if (s == WASessionStateConnected) t = WALoc(@"status_connected");
    else if (s == WASessionStatePairing) t = WALoc(@"status_pairing");
    self.statusSub.text = t;
    self.statusSub.textColor = (s == WASessionStateConnected) ? [WATheme successColor] : [WATheme secondaryActionColor];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)t { return 5; }

- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s
{
    if (s == 0) return 5; // Refresh contacts, Wallpaper, Notification sound, Blocked contacts, Clear all chats
    if (s == 1) return 2; // Encryption, Audio Codec
    if (s == 2) return 1; // Unlink
    if (s == 3) return 1; // Linked Devices
    if (s == 4) return 1; // About
    return 0;
}

- (NSString *)tableView:(UITableView *)t titleForHeaderInSection:(NSInteger)s
{
    if (s == 0) return WALoc(@"chat_settings");
    if (s == 1) return WALoc(@"connection_security");
    if (s == 2) return WALoc(@"profile");
    if (s == 3) return WALoc(@"linked_devices");
    if (s == 4) return WALoc(@"about");
    return nil;
}

- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s
{
    if (s == 2) {
        return [NSString stringWithFormat:@"Wazzup %@\n%@",
                [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"0.1.0",
                @"Not affiliated with WhatsApp / Meta."];
    }
    if (s == 3) {
        return [NSString stringWithFormat:@"Wazzup %@\n%@",
                [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"0.1.0",
                @"Neoficiální WhatsApp klient. Není spojeno s WhatsApp Inc. nebo Meta.\nWhatsApp® je registrovaná značka WhatsApp LLC."];
    }
    return nil;
}

- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip
{
    NSString *cid = [NSString stringWithFormat:@"cell_%ld_%ld", (long)ip.section, (long)ip.row];
    UITableViewCell *cell = [t dequeueReusableCellWithIdentifier:cid];
    if (!cell) {
        UITableViewCellStyle st = (ip.section == 1 || (ip.section == 0 && (ip.row == 1 || ip.row == 2 || ip.row == 3))) ? UITableViewCellStyleValue1 : UITableViewCellStyleDefault;
        cell = [[UITableViewCell alloc] initWithStyle:st reuseIdentifier:cid];
    }
    if (ip.section == 0) {
        if (ip.row == 0) {
            cell.textLabel.text = WALoc(@"refresh_contacts");
            cell.textLabel.textColor = [WATheme accent];
            cell.textLabel.textAlignment = NSTextAlignmentLeft;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        } else if (ip.row == 1) {
            cell.textLabel.text = WALoc(@"wallpaper");
            WAWallpaperType wt = [WATheme currentWallpaperType];
            NSString *wname = WALoc(@"wallpaper_classic");
            if (wt == WAWallpaperTypeParchment) wname = WALoc(@"wallpaper_parchment");
            else if (wt == WAWallpaperTypeSlate) wname = WALoc(@"wallpaper_slate");
            else if (wt == WAWallpaperTypeSky) wname = WALoc(@"wallpaper_sky");
            cell.detailTextLabel.text = wname;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        } else if (ip.row == 2) {
            cell.textLabel.text = WALoc(@"notification_sound");
            NSInteger sid = [[NSUserDefaults standardUserDefaults] integerForKey:@"wa_sound_id"];
            NSString *sname = WALoc(@"sound_default");
            if (sid == 1004) sname = WALoc(@"sound_note");
            else if (sid == 1016) sname = WALoc(@"sound_pop");
            else if (sid == 1000) sname = WALoc(@"sound_bell");
            else if (sid == 1005) sname = WALoc(@"sound_electronic");
            else if (sid == 1024) sname = WALoc(@"sound_fanfare");
            else if (sid == 1021) sname = WALoc(@"sound_chime");
            cell.detailTextLabel.text = sname;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        } else if (ip.row == 3) {
            cell.textLabel.text = WALoc(@"blocked_contacts");
            NSArray *blocked = [[WAChatStore shared] blockedContacts];
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%lu", (unsigned long)blocked.count];
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        } else {
            cell.textLabel.text = WALoc(@"clear_all_chats");
            cell.textLabel.textColor = [WATheme errorColor];
            cell.textLabel.textAlignment = NSTextAlignmentCenter;
            cell.accessoryType = UITableViewCellAccessoryNone;
        }
    } else if (ip.section == 1) {
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        cell.accessoryType = UITableViewCellAccessoryNone;
        if (ip.row == 0) {
            cell.textLabel.text = WALoc(@"encryption");
            cell.detailTextLabel.text = @"Signal + Noise 25519";
        } else {
            cell.textLabel.text = WALoc(@"audio_codecs");
            cell.detailTextLabel.text = @"Meta MLOW + Opus";
        }
    } else if (ip.section == 2) {
        cell.textLabel.text = WALoc(@"unlink");
        cell.textLabel.textColor = [WATheme errorColor];
        cell.textLabel.textAlignment = NSTextAlignmentCenter;
        cell.accessoryType = UITableViewCellAccessoryNone;
    } else if (ip.section == 3) {
        // Linked Devices - show count
        NSArray *devices = [[WADeviceStore shared] pairedDevices];
        cell.textLabel.text = WALoc(@"linked_devices");
        cell.detailTextLabel.text = [NSString stringWithFormat:@"%lu %@", (unsigned long)devices.count, devices.count == 1 ? WALoc(@"device") : WALoc(@"devices")];
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    } else if (ip.section == 4) {
        cell.textLabel.text = WALoc(@"about");
        cell.textLabel.textColor = [WATheme accent];
        cell.textLabel.textAlignment = NSTextAlignmentCenter;
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    }
    return cell;
}

- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)ip
{
    [t deselectRowAtIndexPath:ip animated:YES];
    if (ip.section == 0) {
        if (ip.row == 0) {
            [[WAChatStore shared] refreshContactNames];
            UIAlertView *a = [[UIAlertView alloc] initWithTitle:nil message:WALoc(@"contacts_refreshed") delegate:nil cancelButtonTitle:WALoc(@"ok") otherButtonTitles:nil];
            [a show];
        } else if (ip.row == 1) { // Wallpaper
            UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"wallpaper")
                                                            delegate:self
                                                   cancelButtonTitle:WALoc(@"cancel")
                                              destructiveButtonTitle:nil
                                                   otherButtonTitles:WALoc(@"wallpaper_classic"), WALoc(@"wallpaper_parchment"), WALoc(@"wallpaper_slate"), WALoc(@"wallpaper_sky"), nil];
            as.tag = 503;
            if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
                UITableViewCell *cell = [t cellForRowAtIndexPath:ip];
                [as showFromRect:cell.bounds inView:cell animated:YES];
            } else {
                [as showInView:self.view];
            }
        } else if (ip.row == 2) { // Notification Sound
            UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"notification_sound")
                                                            delegate:self
                                                   cancelButtonTitle:WALoc(@"cancel")
                                              destructiveButtonTitle:nil
                                                   otherButtonTitles:WALoc(@"sound_default"), WALoc(@"sound_note"), WALoc(@"sound_pop"), WALoc(@"sound_bell"), WALoc(@"sound_electronic"), WALoc(@"sound_fanfare"), WALoc(@"sound_chime"), nil];
            as.tag = 504;
            if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
                UITableViewCell *cell = [t cellForRowAtIndexPath:ip];
                [as showFromRect:cell.bounds inView:cell animated:YES];
            } else {
                [as showInView:self.view];
            }
        } else if (ip.row == 3) { // Blocked contacts
            self.blockedList = [[WAChatStore shared] blockedContacts];
            if (self.blockedList.count == 0) {
                UIAlertView *av = [[UIAlertView alloc] initWithTitle:nil message:WALoc(@"no_blocked_contacts") delegate:nil cancelButtonTitle:WALoc(@"ok") otherButtonTitles:nil];
                [av show];
            } else {
                UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"blocked_contacts")
                                                                delegate:self
                                                       cancelButtonTitle:nil
                                                  destructiveButtonTitle:nil
                                                       otherButtonTitles:nil];
                for (NSString *bjid in self.blockedList) {
                    NSString *name = WADisplayNameForChat(bjid, nil);
                    [as addButtonWithTitle:[NSString stringWithFormat:@"%@: %@", WALoc(@"unblock"), name]];
                }
                [as addButtonWithTitle:WALoc(@"cancel")];
                as.cancelButtonIndex = self.blockedList.count;
                as.tag = 505;
                if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
                    UITableViewCell *cell = [t cellForRowAtIndexPath:ip];
                    [as showFromRect:cell.bounds inView:cell animated:YES];
                } else {
                    [as showInView:self.view];
                }
            }
        } else {
            UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"clear_all_chats_confirm")
                                                            delegate:self
                                                   cancelButtonTitle:WALoc(@"cancel")
                                              destructiveButtonTitle:WALoc(@"clear")
                                                   otherButtonTitles:nil];
            as.tag = 501;
            if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
                UITableViewCell *cell = [t cellForRowAtIndexPath:ip];
                [as showFromRect:cell.bounds inView:cell animated:YES];
            } else {
                [as showInView:self.view];
            }
        }
    } else if (ip.section == 2) {
        UIAlertView *a = [[UIAlertView alloc] initWithTitle:WALoc(@"app_name") message:WALoc(@"unlink_confirm")
                                                   delegate:self cancelButtonTitle:WALoc(@"cancel") otherButtonTitles:WALoc(@"unlink"), nil];
        a.tag = 502;
        [a show];
    } else if (ip.section == 3) {
        // Linked Devices - push to device list view controller
        [self showLinkedDevices];
    } else if (ip.section == 4) {
        [self showAbout];
    }
}

- (void)actionSheet:(UIActionSheet *)actionSheet clickedButtonAtIndex:(NSInteger)buttonIndex
{
    if (actionSheet.tag == 501 && buttonIndex == actionSheet.destructiveButtonIndex) {
        [[WAChatStore shared] clearAllChats];
    } else if (actionSheet.tag == 503) { // Wallpaper
        if (buttonIndex >= 0 && buttonIndex <= 3) {
            [WATheme setWallpaperType:(WAWallpaperType)buttonIndex];
            [_table reloadData];
        }
    } else if (actionSheet.tag == 504) { // Sound
        NSArray *sounds = @[ @1003, @1004, @1016, @1000, @1005, @1024, @1021 ];
        if (buttonIndex < sounds.count) {
            NSInteger sid = [sounds[buttonIndex] integerValue];
            [[NSUserDefaults standardUserDefaults] setInteger:sid forKey:@"wa_sound_id"];
            [[NSUserDefaults standardUserDefaults] synchronize];
            AudioServicesPlaySystemSound((SystemSoundID)sid);
            [_table reloadData];
        }
    } else if (actionSheet.tag == 505) { // Unblock contact
        if (buttonIndex < self.blockedList.count) {
            NSString *toUnblock = self.blockedList[buttonIndex];
            [[WAChatStore shared] setContact:toUnblock blocked:NO];
            [[WASession shared] setBlockContact:toUnblock blocked:NO];
            [_table reloadData];
        }
    }
}

- (void)alertView:(UIAlertView *)alertView clickedButtonAtIndex:(NSInteger)buttonIndex
{
    if (alertView.tag == 502 && buttonIndex == 1) {
        [[WASession shared] unlinkAndReset];
        WAAppDelegate *ad = (WAAppDelegate *)[UIApplication sharedApplication].delegate;
        [ad showPairing];
    }
}

- (void)showLinkedDevices
{
    NSArray *devices = [[WADeviceStore shared] pairedDevices];
    
    if (devices.count == 0) {
        UIAlertView *alert = [[UIAlertView alloc] initWithTitle:WALoc(@"linked_devices")
                                                        message:WALoc(@"no_linked_devices")
                                                       delegate:nil
                                              cancelButtonTitle:WALoc(@"ok")
                                              otherButtonTitles:nil];
        [alert show];
        return;
    }
    
    UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"linked_devices")
                                                    delegate:self
                                           cancelButtonTitle:WALoc(@"cancel")
                                      destructiveButtonTitle:nil
                                           otherButtonTitles:nil];
    
    for (NSDictionary *device in devices) {
        NSString *name = device[@"name"];
        NSString *platform = [[WADeviceStore shared] platformNameForPlatform:[device[@"platform"] integerValue]];
        NSString *lastSeen = [self formatLastSeen:device[@"lastSeen"]];
        [as addButtonWithTitle:[NSString stringWithFormat:@"%@ (%@) - %@", name, platform, lastSeen]];
    }
    
    [as addButtonWithTitle:WALoc(@"cancel")];
    as.cancelButtonIndex = devices.count;
    as.tag = 600;
    
    if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
        // Would need a cell reference for iPad
        [as showInView:self.view];
    } else {
        [as showInView:self.view];
    }
}

- (NSString *)formatLastSeen:(NSNumber *)timestamp {
    if (!timestamp) return WALoc(@"never");
    NSTimeInterval ts = [timestamp doubleValue];
    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    NSTimeInterval diff = now - ts;
    
    if (diff < 60) return WALoc(@"just_now");
    if (diff < 3600) return [NSString stringWithFormat:WALoc(@"minutes_ago"), (int)(diff / 60)];
    if (diff < 86400) return [NSString stringWithFormat:WALoc(@"hours_ago"), (int)(diff / 3600)];
    if (diff < 604800) return [NSString stringWithFormat:WALoc(@"days_ago"), (int)(diff / 86400)];
    return WALoc(@"long_time_ago");
}

- (void)showAbout
{
    NSString *msg = @"Wazzup - neoficiální WhatsApp klient\n\n"
                    @"Není spojeno s WhatsApp Inc. nebo Meta.\n\n"
                    @"WhatsApp® je registrovaná značka WhatsApp LLC.\n\n"
                    @"Vyvinuto pro interoperabilitu s WhatsApp protokolem.";
    
    UIAlertView *alert = [[UIAlertView alloc] initWithTitle:@"O aplikaci"
                                                    message:msg
                                                   delegate:nil
                                          cancelButtonTitle:@"OK"
                                          otherButtonTitles:nil];
    [alert show];
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

@end
