#import "WAContactInfoViewController.h"
#import "WACallViewController.h"
#import "WAMediaViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"
#import "../WAChatStore.h"
#import <QuartzCore/QuartzCore.h>
#include "wa_history.h"

@interface WAContactInfoViewController ()
@property (nonatomic, copy) NSString *jid;
@property (nonatomic, copy) NSString *name;
@property (nonatomic, strong) UITableView *table;
@property (nonatomic, strong) UIImageView *avatarView;
@property (nonatomic, strong) UILabel *nameLabel;
@property (nonatomic, strong) UILabel *phoneLabel;
@property (nonatomic, strong) NSArray *messages;
@end

@implementation WAContactInfoViewController

- (instancetype)initWithJID:(NSString *)jid name:(NSString *)name
{
    if ((self = [super init])) {
        _jid = [jid copy];
        _name = [name copy];
    }
    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = WALoc(@"contact_info");
    self.view.backgroundColor = [UIColor colorWithWhite:0.94 alpha:1.0];

    self.table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStyleGrouped];
    self.table.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.table.dataSource = self;
    self.table.delegate = self;

    [self reloadChatData];
    [self setupHeader];

    [self.view addSubview:self.table];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(onStoreChanged:) name:WAChatStoreChanged object:nil];
}

- (void)reloadChatData
{
    self.messages = [[WAChatStore shared] messagesForJID:self.jid];
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
    self.phoneLabel.text = WAFormatPhoneNumber(self.jid);
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
    return 3;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
    if (section == 0) return 2; // Call, Video Call
    if (section == 1) return 3; // Messages count, Media count, Starred count
    if (section == 2) return 2; // Clear Chat, Delete Chat
    return 0;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
    NSString *cellId = [NSString stringWithFormat:@"Cell_%ld_%ld", (long)indexPath.section, (long)indexPath.row];
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:cellId];
    if (!cell) {
        UITableViewCellStyle style = (indexPath.section == 1) ? UITableViewCellStyleValue1 : UITableViewCellStyleDefault;
        cell = [[UITableViewCell alloc] initWithStyle:style reuseIdentifier:cellId];
    }

    if (indexPath.section == 0) {
        if (indexPath.row == 0) {
            cell.textLabel.text = [NSString stringWithFormat:@"☎  %@", WALoc(@"call_voice")];
            cell.textLabel.textColor = [WATheme accent];
        } else {
            cell.textLabel.text = [NSString stringWithFormat:@"▶  %@", WALoc(@"call_video")];
            cell.textLabel.textColor = [WATheme accent];
        }
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    } else if (indexPath.section == 1) {
        if (indexPath.row == 0) {
            cell.textLabel.text = WALoc(@"message_hint");
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%lu %@", (unsigned long)self.messages.count, WALoc(@"messages_count")];
        } else if (indexPath.row == 1) {
            NSUInteger mediaCount = 0;
            for (NSDictionary *m in self.messages) {
                int k = [m[@"kind"] intValue];
                if (k != WA_MSG_TEXT) mediaCount++;
            }
            cell.textLabel.text = WALoc(@"media_shared");
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%lu %@", (unsigned long)mediaCount, WALoc(@"media_count")];
        } else {
            NSUInteger starCount = [[WAChatStore shared] starredMessagesForJID:self.jid].count;
            cell.textLabel.text = [NSString stringWithFormat:@"★  %@", WALoc(@"starred_messages")];
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%lu", (unsigned long)starCount];
        }
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        cell.accessoryType = UITableViewCellAccessoryNone;
    } else if (indexPath.section == 2) {
        if (indexPath.row == 0) {
            cell.textLabel.text = WALoc(@"clear_chat");
            cell.textLabel.textColor = [UIColor colorWithRed:0.85 green:0.2 blue:0.2 alpha:1.0];
        } else {
            cell.textLabel.text = WALoc(@"delete_chat");
            cell.textLabel.textColor = [UIColor colorWithRed:0.85 green:0.2 blue:0.2 alpha:1.0];
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
    } else if (indexPath.section == 2) {
        if (indexPath.row == 0) {
            UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"clear_chat_confirm")
                                                            delegate:self
                                                   cancelButtonTitle:WALoc(@"cancel")
                                              destructiveButtonTitle:WALoc(@"clear")
                                                   otherButtonTitles:nil];
            as.tag = 101;
            if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
                UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
                [as showFromRect:cell.bounds inView:cell animated:YES];
            } else {
                [as showInView:self.view];
            }
        } else if (indexPath.row == 1) {
            UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:WALoc(@"delete_chat_confirm")
                                                            delegate:self
                                                   cancelButtonTitle:WALoc(@"cancel")
                                              destructiveButtonTitle:WALoc(@"delete")
                                                   otherButtonTitles:nil];
            as.tag = 102;
            if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
                UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
                [as showFromRect:cell.bounds inView:cell animated:YES];
            } else {
                [as showInView:self.view];
            }
        }
    }
}

- (void)actionSheet:(UIActionSheet *)actionSheet clickedButtonAtIndex:(NSInteger)buttonIndex
{
    if (actionSheet.tag == 101 && buttonIndex == actionSheet.destructiveButtonIndex) {
        [[WAChatStore shared] clearChat:self.jid];
        [self reloadChatData];
        [self.table reloadData];
    } else if (actionSheet.tag == 102 && buttonIndex == actionSheet.destructiveButtonIndex) {
        [[WAChatStore shared] deleteChat:self.jid];
        [self.navigationController popToRootViewControllerAnimated:YES];
    }
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

@end
