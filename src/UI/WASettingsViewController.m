#import "WASettingsViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"
#import "../WAChatStore.h"
#import "../WAAppDelegate.h"
#import <AudioToolbox/AudioToolbox.h>

@interface WASettingsViewController () <UIActionSheetDelegate>
@property (nonatomic, strong) UILabel *statusSub;
@end

@implementation WASettingsViewController {
    UITableView *_table;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = WALoc(@"settings");
    self.view.backgroundColor = [UIColor colorWithWhite:0.94 alpha:1.0];

    _table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStyleGrouped];
    _table.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _table.dataSource = self;
    _table.delegate = self;

    // profile header: avatar + own number
    NSString *jid = [WASession shared].ownJID ?: @"";
    if (jid.length) {
        NSString *key = [WAChatStore keyForJID:jid];
        NSString *p = [WADocumentsPath() stringByAppendingPathComponent:[NSString stringWithFormat:@"wa6_avatar_%@.jpg", key]];
        if (![[NSFileManager defaultManager] fileExistsAtPath:p]) {
            [[WASession shared] fetchProfilePicture:jid];
        }
    }
    NSString *user = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
    NSString *num = [user componentsSeparatedByString:@":"].firstObject;
    CGFloat w = self.view.bounds.size.width;
    UIView *hdr = [[UIView alloc] initWithFrame:CGRectMake(0, 0, w, 108)];
    UIImageView *av = [[UIImageView alloc] initWithFrame:CGRectMake(18, 22, 64, 64)];
    av.image = [WATheme avatarForName:WALoc(@"app_name") jid:(jid.length ? jid : @"wazzup") diameter:64];
    [hdr addSubview:av];
    UILabel *nl = [[UILabel alloc] initWithFrame:CGRectMake(96, 36, w - 110, 26)];
    nl.font = [UIFont boldSystemFontOfSize:21];
    nl.text = num.length ? [@"+" stringByAppendingString:num] : WALoc(@"profile");
    nl.backgroundColor = [UIColor clearColor];
    [hdr addSubview:nl];
    self.statusSub = [[UILabel alloc] initWithFrame:CGRectMake(96, 64, w - 110, 20)];
    self.statusSub.font = [UIFont systemFontOfSize:14];
    self.statusSub.textColor = [UIColor colorWithWhite:0.5 alpha:1.0];
    self.statusSub.backgroundColor = [UIColor clearColor];
    [hdr addSubview:self.statusSub];
    _table.tableHeaderView = hdr;

    [self.view addSubview:_table];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(updateStatusText) name:WASessionStateChanged object:nil];
    [self updateStatusText];
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
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)t { return 3; }

- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s
{
    if (s == 0) return 4; // Refresh contacts, Wallpaper, Notification sound, Clear all chats
    if (s == 1) return 2; // Encryption, Audio Codec
    if (s == 2) return 1; // Unlink
    return 0;
}

- (NSString *)tableView:(UITableView *)t titleForHeaderInSection:(NSInteger)s
{
    if (s == 0) return WALoc(@"chat_settings");
    if (s == 1) return WALoc(@"connection_security");
    if (s == 2) return WALoc(@"profile");
    return nil;
}

- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s
{
    if (s == 2) {
        return [NSString stringWithFormat:@"Wazzup %@\n%@",
                [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"0.1.0",
                @"Not affiliated with WhatsApp / Meta."];
    }
    return nil;
}

- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip
{
    NSString *cid = [NSString stringWithFormat:@"cell_%ld_%ld", (long)ip.section, (long)ip.row];
    UITableViewCell *cell = [t dequeueReusableCellWithIdentifier:cid];
    if (!cell) {
        UITableViewCellStyle st = (ip.section == 1 || (ip.section == 0 && (ip.row == 1 || ip.row == 2))) ? UITableViewCellStyleValue1 : UITableViewCellStyleDefault;
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
        } else {
            cell.textLabel.text = WALoc(@"clear_all_chats");
            cell.textLabel.textColor = [UIColor colorWithRed:0.85 green:0.2 blue:0.2 alpha:1.0];
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
        cell.textLabel.textColor = [UIColor colorWithRed:0.85 green:0.1 blue:0.1 alpha:1.0];
        cell.textLabel.textAlignment = NSTextAlignmentCenter;
        cell.accessoryType = UITableViewCellAccessoryNone;
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

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

@end
