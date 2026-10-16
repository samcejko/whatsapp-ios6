#import "WASettingsViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"
#import "../WAChatStore.h"
#import "../WAAppDelegate.h"

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
    UILabel *sub = [[UILabel alloc] initWithFrame:CGRectMake(96, 64, w - 110, 20)];
    sub.font = [UIFont systemFontOfSize:14];
    sub.textColor = [UIColor colorWithWhite:0.5 alpha:1.0];
    sub.text = WALoc(@"status_connected");
    sub.backgroundColor = [UIColor clearColor];
    [hdr addSubview:sub];
    _table.tableHeaderView = hdr;

    [self.view addSubview:_table];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)t { return 1; }
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return 1; }

- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s
{
    return [NSString stringWithFormat:@"Wazzup %@\n%@",
            [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"",
            @"Not affiliated with WhatsApp / Meta."];
}

- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip
{
    UITableViewCell *cell = [t dequeueReusableCellWithIdentifier:@"s"];
    if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"s"];
    cell.textLabel.text = WALoc(@"unlink");
    cell.textLabel.textColor = [UIColor colorWithRed:0.8 green:0.1 blue:0.1 alpha:1.0];
    cell.textLabel.textAlignment = NSTextAlignmentCenter;
    return cell;
}

- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)ip
{
    [t deselectRowAtIndexPath:ip animated:YES];
    UIAlertView *a = [[UIAlertView alloc] initWithTitle:WALoc(@"app_name") message:WALoc(@"unlink_confirm")
                                               delegate:self cancelButtonTitle:WALoc(@"cancel") otherButtonTitles:WALoc(@"unlink"), nil];
    [a show];
}

- (void)alertView:(UIAlertView *)alertView clickedButtonAtIndex:(NSInteger)buttonIndex
{
    if (buttonIndex == 1) {
        [[WASession shared] unlinkAndReset];
        WAAppDelegate *ad = (WAAppDelegate *)[UIApplication sharedApplication].delegate;
        [ad showPairing];
    }
}

@end
