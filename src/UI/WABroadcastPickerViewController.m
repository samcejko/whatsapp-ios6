#import "WABroadcastPickerViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WAChatStore.h"

@interface WABroadcastPickerViewController ()
@property (nonatomic, strong) UITableView *table;
@property (nonatomic, strong) NSMutableArray *candidates;
@property (nonatomic, strong) NSMutableSet *selectedJIDs;
@end

@implementation WABroadcastPickerViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = WALoc(@"new_broadcast");
    self.view.backgroundColor = [UIColor whiteColor];

    self.selectedJIDs = [NSMutableSet set];
    self.candidates = [NSMutableArray array];

    NSArray *all = [[WAChatStore shared] conversationsSorted];
    for (NSDictionary *c in all) {
        NSString *jid = c[@"jid"];
        if (!jid.length || [jid hasSuffix:@"@broadcast"]) continue;
        [self.candidates addObject:c];
    }

    self.table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStylePlain];
    self.table.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.rowHeight = 60;
    [self.view addSubview:self.table];

    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:WALoc(@"cancel") style:UIBarButtonItemStylePlain target:self action:@selector(cancelTapped)];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:WALoc(@"save") style:UIBarButtonItemStyleDone target:self action:@selector(doneTapped)];
    self.navigationItem.rightBarButtonItem.enabled = NO;

    [self updateTitle];
}

- (void)updateTitle
{
    NSUInteger count = self.selectedJIDs.count;
    if (count == 0) {
        self.title = WALoc(@"new_broadcast");
        self.navigationItem.rightBarButtonItem.enabled = NO;
    } else {
        self.title = [NSString stringWithFormat:@"%@ (%lu)", WALoc(@"broadcast_recipients"), (unsigned long)count];
        self.navigationItem.rightBarButtonItem.enabled = YES;
    }
}

- (void)cancelTapped
{
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)doneTapped
{
    if (self.selectedJIDs.count > 0 && [self.delegate respondsToSelector:@selector(broadcastPickerDidFinishWithRecipients:)]) {
        [self.delegate broadcastPickerDidFinishWithRecipients:[self.selectedJIDs allObjects]];
    }
    [self dismissViewControllerAnimated:YES completion:nil];
}

#pragma mark - Table View

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
    return self.candidates.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
    static NSString *cid = @"BroadcastCell";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:cid];
    if (!cell) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:cid];
    }
    NSDictionary *row = self.candidates[indexPath.row];
    NSString *jid = row[@"jid"];
    NSString *name = WADisplayNameForChat(jid, row[@"name"]);

    cell.textLabel.text = name;
    cell.textLabel.font = [UIFont boldSystemFontOfSize:16];
    cell.detailTextLabel.text = WAFormatPhoneNumber(jid);
    cell.imageView.image = [WATheme avatarForName:name jid:jid diameter:44];

    BOOL selected = [self.selectedJIDs containsObject:jid];
    cell.accessoryType = selected ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;

    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSDictionary *row = self.candidates[indexPath.row];
    NSString *jid = row[@"jid"];
    if (!jid.length) return;

    if ([self.selectedJIDs containsObject:jid]) {
        [self.selectedJIDs removeObject:jid];
    } else {
        [self.selectedJIDs addObject:jid];
    }
    [self.table reloadRowsAtIndexPaths:@[indexPath] withRowAnimation:UITableViewRowAnimationNone];
    [self updateTitle];
}

@end
