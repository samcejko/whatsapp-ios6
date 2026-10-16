#import "WAChatViewController.h"
#import "WACallViewController.h"
#import "WAMediaViewController.h"
#import "WAContactInfoViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"
#import "../WAChatStore.h"
#import "../WAAudioPlayer.h"
#import <QuartzCore/QuartzCore.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include "wa_history.h"   // WA_MSG_* kinds

@interface WAChatViewController () <UIDocumentInteractionControllerDelegate, UIActionSheetDelegate, UISearchBarDelegate, AVAudioRecorderDelegate>
@property (nonatomic, copy) NSString *jid;
@property (nonatomic) BOOL isGroup;
@property (nonatomic, strong) NSMutableArray *messages;   // all messages
@property (nonatomic, strong) NSMutableArray *filteredMessages; // search results
@property (nonatomic) BOOL isSearching;
@property (nonatomic, copy) NSString *searchQuery;

@property (nonatomic, strong) UITableView *table;
@property (nonatomic, strong) UISearchBar *chatSearchBar;

@property (nonatomic, strong) UIView *bar;
@property (nonatomic, strong) UIButton *attach;
@property (nonatomic, strong) UITextField *field;
@property (nonatomic, strong) UIButton *send;
@property (nonatomic, strong) UIButton *micBtn;

@property (nonatomic, strong) UIView *replyBar;
@property (nonatomic, strong) UILabel *replyAuthorLabel;
@property (nonatomic, strong) UILabel *replyTextLabel;
@property (nonatomic, strong) UIButton *replyCancelBtn;
@property (nonatomic, strong) NSDictionary *replyingToMsg;

@property (nonatomic, strong) UIPopoverController *popover;
@property (nonatomic) CGFloat kbHeight;
@property (nonatomic, strong) UIDocumentInteractionController *docController;
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UILabel *subtitleLabel;
@property (nonatomic, strong) NSDictionary *selectedMsg;
@property (nonatomic, strong) NSArray *forwardCandidates;

// Voice recording
@property (nonatomic) BOOL isRecordingVoice;
@property (nonatomic, strong) AVAudioRecorder *recorder;
@property (nonatomic, strong) NSString *recordFilePath;
@property (nonatomic) NSTimeInterval recordStartTime;
@property (nonatomic, strong) NSTimer *recordTimer;
@end

@implementation WAChatViewController

- (instancetype)initWithJID:(NSString *)jid name:(NSString *)name
{
    if ((self = [super init])) {
        _jid = [jid copy];
        _isGroup = [jid rangeOfString:@"@g.us"].location != NSNotFound;
        self.title = WADisplayNameForChat(jid, name);
        _messages = [NSMutableArray array];
        _filteredMessages = [NSMutableArray array];
    }
    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [[UIColor alloc] initWithPatternImage:[WATheme wallpaper]];

    // Nav Bar Buttons: Search, Video Call, Voice Call
    UIButton *call = [UIButton buttonWithType:UIButtonTypeCustom];
    [call setImage:[WATheme roundCallButton:[WATheme accent] diameter:30 glyph:@"☎"] forState:UIControlStateNormal];
    call.frame = CGRectMake(0, 0, 30, 30);
    [call addTarget:self action:@selector(voiceCall) forControlEvents:UIControlEventTouchUpInside];

    UIButton *video = [UIButton buttonWithType:UIButtonTypeCustom];
    [video setImage:[WATheme roundCallButton:[WATheme accent] diameter:30 glyph:@"▶"] forState:UIControlStateNormal];
    video.frame = CGRectMake(0, 0, 30, 30);
    [video addTarget:self action:@selector(videoCall) forControlEvents:UIControlEventTouchUpInside];

    UIBarButtonItem *searchItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemSearch target:self action:@selector(toggleSearch)];

    self.navigationItem.rightBarButtonItems = @[ [[UIBarButtonItem alloc] initWithCustomView:video],
                                                 [[UIBarButtonItem alloc] initWithCustomView:call],
                                                 searchItem ];

    // Title View: Contact Name + Ephemeral indicator + Presence / Recording status
    UIView *tv = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 180, 44)];
    tv.userInteractionEnabled = YES;
    [tv addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(openContactInfo)]];

    self.titleLabel = [[UILabel alloc] initWithFrame:CGRectMake(0, 4, 180, 20)];
    BOOL isEphemeral = [[WAChatStore shared] ephemeralDurationForChat:self.jid] > 0;
    self.titleLabel.text = isEphemeral ? [NSString stringWithFormat:@"⏱ %@", self.title] : self.title;
    self.titleLabel.font = [UIFont boldSystemFontOfSize:17];
    self.titleLabel.textColor = [UIColor whiteColor];
    self.titleLabel.textAlignment = NSTextAlignmentCenter;
    self.titleLabel.backgroundColor = [UIColor clearColor];
    [tv addSubview:self.titleLabel];

    self.subtitleLabel = [[UILabel alloc] initWithFrame:CGRectMake(0, 24, 180, 16)];
    self.subtitleLabel.font = [UIFont systemFontOfSize:11];
    self.subtitleLabel.textColor = [UIColor colorWithWhite:0.92 alpha:1.0];
    self.subtitleLabel.textAlignment = NSTextAlignmentCenter;
    self.subtitleLabel.backgroundColor = [UIColor clearColor];
    [tv addSubview:self.subtitleLabel];
    self.navigationItem.titleView = tv;

    // Search bar for in-chat search
    self.chatSearchBar = [[UISearchBar alloc] initWithFrame:CGRectMake(0, 0, self.view.bounds.size.width, 44)];
    self.chatSearchBar.placeholder = WALoc(@"search_in_chat");
    self.chatSearchBar.delegate = self;
    self.chatSearchBar.hidden = YES;
    [self.view addSubview:self.chatSearchBar];

    // Table view
    self.table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStylePlain];
    self.table.backgroundColor = [UIColor clearColor];
    self.table.separatorStyle = UITableViewCellSeparatorStyleNone;
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.allowsSelection = YES;
    [self.table addGestureRecognizer:[[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(onLongPress:)]];
    [self.view addSubview:self.table];

    // Reply preview banner (above input bar)
    self.replyBar = [[UIView alloc] init];
    self.replyBar.backgroundColor = [UIColor colorWithWhite:0.95 alpha:1.0];
    self.replyBar.hidden = YES;

    UIView *replyLine = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 4, 40)];
    replyLine.backgroundColor = [WATheme accent];
    [self.replyBar addSubview:replyLine];

    self.replyAuthorLabel = [[UILabel alloc] initWithFrame:CGRectMake(12, 3, self.view.bounds.size.width - 50, 16)];
    self.replyAuthorLabel.font = [UIFont boldSystemFontOfSize:12];
    self.replyAuthorLabel.textColor = [WATheme accent];
    self.replyAuthorLabel.backgroundColor = [UIColor clearColor];
    [self.replyBar addSubview:self.replyAuthorLabel];

    self.replyTextLabel = [[UILabel alloc] initWithFrame:CGRectMake(12, 20, self.view.bounds.size.width - 50, 16)];
    self.replyTextLabel.font = [UIFont systemFontOfSize:12];
    self.replyTextLabel.textColor = [UIColor colorWithWhite:0.4 alpha:1.0];
    self.replyTextLabel.backgroundColor = [UIColor clearColor];
    [self.replyBar addSubview:self.replyTextLabel];

    self.replyCancelBtn = [UIButton buttonWithType:UIButtonTypeCustom];
    [self.replyCancelBtn setTitle:@"✕" forState:UIControlStateNormal];
    [self.replyCancelBtn setTitleColor:[UIColor colorWithWhite:0.5 alpha:1.0] forState:UIControlStateNormal];
    self.replyCancelBtn.titleLabel.font = [UIFont boldSystemFontOfSize:16];
    [self.replyCancelBtn addTarget:self action:@selector(cancelReply) forControlEvents:UIControlEventTouchUpInside];
    [self.replyBar addSubview:self.replyCancelBtn];

    [self.view addSubview:self.replyBar];

    // Input bar
    self.bar = [[UIView alloc] init];
    self.bar.backgroundColor = [UIColor colorWithWhite:0.93 alpha:1.0];
    [self.view addSubview:self.bar];

    self.attach = [UIButton buttonWithType:UIButtonTypeCustom];
    [self.attach setTitle:@"+" forState:UIControlStateNormal];
    [self.attach setTitleColor:[UIColor colorWithWhite:0.35 alpha:1.0] forState:UIControlStateNormal];
    self.attach.titleLabel.font = [UIFont boldSystemFontOfSize:22];
    [self.attach setBackgroundImage:[WATheme glossyButtonWithColor:[UIColor colorWithWhite:0.85 alpha:1.0]] forState:UIControlStateNormal];
    [self.attach addTarget:self action:@selector(attachTapped) forControlEvents:UIControlEventTouchUpInside];
    [self.bar addSubview:self.attach];

    self.field = [[UITextField alloc] init];
    self.field.borderStyle = UITextBorderStyleRoundedRect;
    self.field.placeholder = WALoc(@"message_hint");
    self.field.font = [UIFont systemFontOfSize:16];
    self.field.returnKeyType = UIReturnKeySend;
    self.field.delegate = self;
    [self.field addTarget:self action:@selector(fieldTextChanged:) forControlEvents:UIControlEventEditingChanged];
    [self.bar addSubview:self.field];

    self.send = [UIButton buttonWithType:UIButtonTypeCustom];
    [self.send setTitle:WALoc(@"send") forState:UIControlStateNormal];
    [self.send setBackgroundImage:[WATheme glossyButtonWithColor:[WATheme accent]] forState:UIControlStateNormal];
    self.send.titleLabel.font = [UIFont boldSystemFontOfSize:15];
    [self.send addTarget:self action:@selector(sendTapped) forControlEvents:UIControlEventTouchUpInside];
    self.send.hidden = YES;
    [self.bar addSubview:self.send];

    // Microphone button for voice notes
    self.micBtn = [UIButton buttonWithType:UIButtonTypeCustom];
    [self.micBtn setTitle:@"🎤" forState:UIControlStateNormal];
    [self.micBtn setBackgroundImage:[WATheme glossyButtonWithColor:[WATheme accent]] forState:UIControlStateNormal];
    self.micBtn.titleLabel.font = [UIFont systemFontOfSize:18];
    [self.micBtn addTarget:self action:@selector(micTouchDown) forControlEvents:UIControlEventTouchDown];
    [self.micBtn addTarget:self action:@selector(micTouchUpInside) forControlEvents:UIControlEventTouchUpInside];
    [self.micBtn addTarget:self action:@selector(micTouchUpOutside) forControlEvents:UIControlEventTouchUpOutside];
    [self.bar addSubview:self.micBtn];

    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(kbShow:) name:UIKeyboardWillShowNotification object:nil];
    [nc addObserver:self selector:@selector(kbHide:) name:UIKeyboardWillHideNotification object:nil];
    [nc addObserver:self selector:@selector(onStoreChanged:) name:WAChatStoreChanged object:nil];
    [nc addObserver:self selector:@selector(onAudioPlayerState:) name:WAAudioPlayerStateChanged object:nil];
    [nc addObserver:self selector:@selector(onPresence:) name:WASessionPresenceChanged object:nil];
    [nc addObserver:self selector:@selector(onWallpaperChanged:) name:@"WAWallpaperChangedNotification" object:nil];

    [[WASession shared] subscribePresence:self.jid];
    NSString *key = [WAChatStore keyForJID:self.jid];
    NSString *avatarPath = [WADocumentsPath() stringByAppendingPathComponent:[NSString stringWithFormat:@"wa6_avatar_%@.jpg", key]];
    if (![[NSFileManager defaultManager] fileExistsAtPath:avatarPath]) {
        [[WASession shared] fetchProfilePicture:self.jid];
    }

    [self loadMessages];
    [self.table reloadData];
    [self scrollToBottom];
}

- (void)onWallpaperChanged:(NSNotification *)n
{
    self.view.backgroundColor = [[UIColor alloc] initWithPatternImage:[WATheme wallpaper]];
}

- (void)viewWillAppear:(BOOL)animated
{
    [super viewWillAppear:animated];
    [[WAChatStore shared] markRead:self.jid];
    NSArray *msgs = [[WAChatStore shared] messagesForJID:self.jid];
    for (NSDictionary *m in msgs) {
        if (![m[@"out"] boolValue] && m[@"id"]) {
            [[WASession shared] sendReadReceiptForMessage:m[@"id"] toJID:self.jid participant:m[@"sender"]];
        }
    }
}

- (void)viewWillDisappear:(BOOL)animated
{
    [super viewWillDisappear:animated];
    [[WAAudioPlayer shared] stop];
    [[WASession shared] unsubscribePresence:self.jid];
    [[WASession shared] sendChatState:NO toJID:self.jid];
}

#pragma mark - Search

- (void)toggleSearch
{
    self.isSearching = !self.isSearching;
    self.chatSearchBar.hidden = !self.isSearching;
    if (self.isSearching) {
        [self.chatSearchBar becomeFirstResponder];
    } else {
        self.searchQuery = @"";
        self.chatSearchBar.text = @"";
        [self.chatSearchBar resignFirstResponder];
        [self applySearchFilter];
    }
    [self.view setNeedsLayout];
    [self.table reloadData];
}

- (void)searchBar:(UISearchBar *)searchBar textDidChange:(NSString *)searchText
{
    self.searchQuery = searchText;
    [self applySearchFilter];
}

- (void)searchBarCancelButtonClicked:(UISearchBar *)searchBar
{
    [self toggleSearch];
}

- (void)searchBarSearchButtonClicked:(UISearchBar *)searchBar
{
    [searchBar resignFirstResponder];
}

- (void)applySearchFilter
{
    [self.filteredMessages removeAllObjects];
    if (self.searchQuery.length) {
        NSString *q = [self.searchQuery lowercaseString];
        for (NSDictionary *m in self.messages) {
            NSString *d = [m[@"display"] lowercaseString];
            if ([d rangeOfString:q].location != NSNotFound) {
                [self.filteredMessages addObject:m];
            }
        }
    }
    [self.table reloadData];
}

- (NSArray *)activeMessages
{
    return (self.isSearching && self.searchQuery.length) ? self.filteredMessages : self.messages;
}

#pragma mark - Reply

- (void)cancelReply
{
    self.replyingToMsg = nil;
    self.replyBar.hidden = YES;
    [self.view setNeedsLayout];
}

#pragma mark - Field text changes & Voice Recording

- (void)fieldTextChanged:(UITextField *)tf
{
    BOOL hasText = tf.text.length > 0;
    self.send.hidden = !hasText;
    self.micBtn.hidden = hasText;
    if (hasText) {
        [[WASession shared] sendChatState:YES toJID:self.jid];
    }
}

- (void)micTouchDown
{
    self.isRecordingVoice = YES;
    self.field.placeholder = WALoc(@"recording");
    [[WASession shared] sendRecordingState:YES toJID:self.jid];

    self.recordStartTime = [NSDate timeIntervalSinceReferenceDate];
    self.recordTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 target:self selector:@selector(recordTick) userInfo:nil repeats:YES];

    NSString *mid = [NSString stringWithFormat:@"3EB0%08X%08X", (unsigned int)arc4random(), (unsigned int)arc4random()];
    NSString *basePath = [WADocumentsPath() stringByAppendingPathComponent:[NSString stringWithFormat:@"wa6_aud_%@", mid]];
    self.recordFilePath = [basePath stringByAppendingPathExtension:@"wav"];

    NSDictionary *settings = @{
        AVFormatIDKey: @(kAudioFormatLinearPCM),
        AVSampleRateKey: @(16000.0),
        AVNumberOfChannelsKey: @(1),
        AVLinearPCMBitDepthKey: @(16),
        AVLinearPCMIsBigEndianKey: @NO,
        AVLinearPCMIsFloatKey: @NO
    };

    NSError *err = nil;
    self.recorder = [[AVAudioRecorder alloc] initWithURL:[NSURL fileURLWithPath:self.recordFilePath] settings:settings error:&err];
    self.recorder.delegate = self;
    [self.recorder record];
}

- (void)recordTick
{
    int secs = (int)([NSDate timeIntervalSinceReferenceDate] - self.recordStartTime);
    self.field.placeholder = [NSString stringWithFormat:@"🔴  %d:%02d  (%@)", secs / 60, secs % 60, WALoc(@"release_to_send")];
}

- (void)micTouchUpInside
{
    if (!self.isRecordingVoice) return;
    self.isRecordingVoice = NO;
    [self.recordTimer invalidate];
    self.recordTimer = nil;
    [self.recorder stop];
    self.recorder = nil;
    [[WASession shared] sendRecordingState:NO toJID:self.jid];
    self.field.placeholder = WALoc(@"message_hint");

    NSTimeInterval dur = [NSDate timeIntervalSinceReferenceDate] - self.recordStartTime;
    if (dur >= 1.0) {
        // Record outgoing voice note in store
        NSString *mid = [NSString stringWithFormat:@"3EB0%08X%08X", (unsigned int)arc4random(), (unsigned int)arc4random()];
        NSMutableDictionary *e = [[WAChatStore shared] entryForJID:self.jid create:YES];
        unsigned long long ts = (unsigned long long)[[NSDate date] timeIntervalSince1970];
        NSMutableDictionary *m = [NSMutableDictionary dictionaryWithObjectsAndKeys:
                                  @"", @"text",
                                  @YES, @"out",
                                  @(ts), @"ts",
                                  mid, @"id",
                                  @(WA_MSG_AUDIO), @"kind",
                                  @((uint32_t)ceil(dur)), @"duration",
                                  @(WAMessageStatusSent), @"status", nil];
        [[WAChatStore shared] mergeMessages:@[ m ] into:e];
        e[@"ts"] = @(ts);
        [[WAChatStore shared] save];
        [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": [WAChatStore keyForJID:self.jid] }];
        AudioServicesPlaySystemSound(1004);
    } else {
        [[NSFileManager defaultManager] removeItemAtPath:self.recordFilePath error:nil];
    }
}

- (void)micTouchUpOutside
{
    if (!self.isRecordingVoice) return;
    self.isRecordingVoice = NO;
    [self.recordTimer invalidate];
    self.recordTimer = nil;
    [self.recorder stop];
    [self.recorder deleteRecording];
    self.recorder = nil;
    [[WASession shared] sendRecordingState:NO toJID:self.jid];
    self.field.placeholder = WALoc(@"message_hint");
}

#pragma mark - Presence

- (void)onPresence:(NSNotification *)n
{
    NSString *jid = n.userInfo[@"jid"];
    if (![jid isEqualToString:self.jid]) return;
    NSString *state = n.userInfo[@"state"];
    if ([state isEqualToString:@"recording"]) {
        self.subtitleLabel.text = WALoc(@"recording_audio");
        self.subtitleLabel.textColor = [WATheme accent];
    } else if ([state isEqualToString:@"composing"]) {
        self.subtitleLabel.text = WALoc(@"typing");
        self.subtitleLabel.textColor = [WATheme accent];
    } else if ([state isEqualToString:@"available"]) {
        self.subtitleLabel.text = WALoc(@"online");
        self.subtitleLabel.textColor = [UIColor colorWithWhite:0.92 alpha:1.0];
    } else {
        self.subtitleLabel.text = @"";
    }
}

- (void)onAudioPlayerState:(NSNotification *)n
{
    [self.table reloadData];
}

#pragma mark - Load Messages

- (void)loadMessages
{
    [self.messages removeAllObjects];
    NSArray *raw = [[WAChatStore shared] messagesForJID:self.jid];
    NSString *lastDay = nil;
    for (NSDictionary *m in raw) {
        unsigned long long ts = [m[@"ts"] unsignedLongLongValue];
        NSString *day = WADayLabel(ts);
        if (day.length && ![day isEqualToString:lastDay]) {
            [self.messages addObject:@{ @"sep": day }];
            lastDay = day;
        }
        NSMutableDictionary *d = [m mutableCopy];
        int kind = [m[@"kind"] intValue];
        NSString *label = [WAChatStore mediaLabelForKind:kind];
        NSString *text = m[@"text"];
        d[@"display"] = (text.length ? text : (label.length ? label : @""));
        NSData *thumb = m[@"thumb"];
        if ([thumb isKindOfClass:[NSData class]] && thumb.length) {
            UIImage *img = [UIImage imageWithData:thumb];
            if (img) d[@"image"] = img;
        }
        [self.messages addObject:d];
    }
    if (self.isSearching) [self applySearchFilter];
}

- (NSString *)displayTextFor:(NSDictionary *)m
{
    int kind = [m[@"kind"] intValue];
    if (kind == WA_MSG_AUDIO) {
        NSString *mid = m[@"id"] ?: [NSString stringWithFormat:@"ts_%@", m[@"ts"]];
        WAAudioPlayer *player = [WAAudioPlayer shared];
        BOOL isThis = [player.currentMsgId isEqualToString:mid];
        NSString *stateIcon = (isThis && player.isPlaying) ? @"⏸ " : @"▶ ";
        uint32_t dur = [m[@"duration"] unsignedIntValue];
        if (dur == 0) dur = 1;
        if (isThis && (player.isPlaying || player.isPaused)) {
            int cur = (int)player.currentTime;
            int total = (int)player.duration;
            if (total == 0) total = dur;
            return [NSString stringWithFormat:@"%@ %d:%02d / %d:%02d", stateIcon, cur / 60, cur % 60, total / 60, total % 60];
        }
        return [NSString stringWithFormat:@"%@ %@ (%d:%02d)", stateIcon, WALoc(@"media_voice"), dur / 60, dur % 60];
    }
    return m[@"display"] ?: @"";
}

- (CGSize)thumbSizeFor:(NSDictionary *)m
{
    UIImage *img = m[@"image"];
    if (!img || img.size.width < 1 || img.size.height < 1) return CGSizeZero;
    CGFloat maxW = MIN(self.view.bounds.size.width * 0.6, 220), maxH = 240;
    CGFloat scale = MIN(MIN(maxW / img.size.width, maxH / img.size.height), 1.0);
    return CGSizeMake(floorf(img.size.width * scale), floorf(img.size.height * scale));
}

- (void)onStoreChanged:(NSNotification *)n
{
    NSString *changed = n.userInfo[@"jid"];
    if (changed.length && ![changed isEqualToString:[WAChatStore keyForJID:self.jid]]) return;
    [self loadMessages];
    [self.table reloadData];
    [self scrollToBottom];
}

#pragma mark - Layout

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    CGFloat w = self.view.bounds.size.width, h = self.view.bounds.size.height;
    CGFloat barH = 46;
    CGFloat topOffset = self.isSearching ? 44 : 0;
    CGFloat replyH = self.replyingToMsg ? 40 : 0;

    if (self.isSearching) {
        self.chatSearchBar.frame = CGRectMake(0, 0, w, 44);
    }

    if (self.replyingToMsg) {
        self.replyBar.frame = CGRectMake(0, h - barH - replyH - self.kbHeight, w, replyH);
        self.replyAuthorLabel.frame = CGRectMake(12, 3, w - 50, 16);
        self.replyTextLabel.frame = CGRectMake(12, 20, w - 50, 16);
        self.replyCancelBtn.frame = CGRectMake(w - 36, 5, 30, 30);
        self.replyBar.hidden = NO;
    } else {
        self.replyBar.hidden = YES;
    }

    self.bar.frame = CGRectMake(0, h - barH - self.kbHeight, w, barH);
    CGFloat btnW = 34;
    self.attach.frame = CGRectMake(8, 7, btnW, barH - 14);
    CGFloat fx = 8 + btnW + 6;
    CGFloat actionBtnW = 60;
    self.field.frame = CGRectMake(fx, 7, w - fx - actionBtnW - 8, barH - 14);
    self.send.frame = CGRectMake(w - actionBtnW - 8, 7, actionBtnW, barH - 14);
    self.micBtn.frame = CGRectMake(w - actionBtnW - 8, 7, actionBtnW, barH - 14);

    self.table.frame = CGRectMake(0, topOffset, w, h - topOffset - barH - replyH - self.kbHeight);
}

#pragma mark - Attach & Camera

- (void)attachTapped
{
    UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:nil
                                                    delegate:self
                                           cancelButtonTitle:WALoc(@"cancel")
                                      destructiveButtonTitle:nil
                                           otherButtonTitles:WALoc(@"take_photo"), WALoc(@"choose_photo"), nil];
    as.tag = 300;
    if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
        [as showFromRect:self.attach.bounds inView:self.attach animated:YES];
    } else {
        [as showInView:self.view];
    }
}

- (void)imagePickerController:(UIImagePickerController *)picker didFinishPickingMediaWithInfo:(NSDictionary *)info
{
    UIImage *orig = info[UIImagePickerControllerOriginalImage];
    if (orig) {
        CGSize sz = orig.size;
        CGFloat fullMax = 1280.0;
        CGFloat fullScale = (sz.width > fullMax || sz.height > fullMax) ? MIN(fullMax / sz.width, fullMax / sz.height) : 1.0;
        CGSize fullSz = CGSizeMake(floorf(sz.width * fullScale), floorf(sz.height * fullScale));
        UIGraphicsBeginImageContextWithOptions(fullSz, NO, 1.0);
        [orig drawInRect:CGRectMake(0, 0, fullSz.width, fullSz.height)];
        UIImage *fullImg = UIGraphicsGetImageFromCurrentImageContext();
        UIGraphicsEndImageContext();
        NSData *jpegData = UIImageJPEGRepresentation(fullImg, 0.8);

        CGFloat maxD = 160.0;
        CGFloat scale = (sz.width > maxD || sz.height > maxD) ? MIN(maxD / sz.width, maxD / sz.height) : 1.0;
        CGSize newSz = CGSizeMake(floorf(sz.width * scale), floorf(sz.height * scale));
        UIGraphicsBeginImageContextWithOptions(newSz, NO, 1.0);
        [orig drawInRect:CGRectMake(0, 0, newSz.width, newSz.height)];
        UIImage *thumbImg = UIGraphicsGetImageFromCurrentImageContext();
        UIGraphicsEndImageContext();
        NSData *thumbData = UIImageJPEGRepresentation(thumbImg, 0.7);

        NSString *mid = [NSString stringWithFormat:@"3EB0%08X%08X", (unsigned int)arc4random(), (unsigned int)arc4random()];
        [[WAChatStore shared] recordOutgoingMedia:thumbData caption:@"" toJID:self.jid msgId:mid status:WAMessageStatusPending];
        [[WASession shared] sendImage:jpegData thumb:thumbData caption:@"" toJID:self.jid msgId:mid];
        AudioServicesPlaySystemSound(1004);
        WALog(@"send image to %@ [%@]: %lu bytes", self.jid, mid, (unsigned long)jpegData.length);
    }
    if (self.popover) {
        [self.popover dismissPopoverAnimated:YES];
        self.popover = nil;
    } else {
        [picker dismissViewControllerAnimated:YES completion:nil];
    }
}

- (void)imagePickerControllerDidCancel:(UIImagePickerController *)picker
{
    if (self.popover) {
        [self.popover dismissPopoverAnimated:YES];
        self.popover = nil;
    } else {
        [picker dismissViewControllerAnimated:YES completion:nil];
    }
}

#pragma mark - Keyboard

- (void)kbShow:(NSNotification *)n
{
    CGRect r = [n.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    CGFloat kh = [self.view convertRect:r fromView:nil].size.height;
    if (kh > 0 && kh != self.kbHeight) {
        self.kbHeight = kh;
        [UIView animateWithDuration:0.25 animations:^{ [self.view setNeedsLayout]; [self.view layoutIfNeeded]; }];
        [self scrollToBottom];
    }
}

- (void)kbHide:(NSNotification *)n
{
    self.kbHeight = 0;
    [UIView animateWithDuration:0.25 animations:^{ [self.view setNeedsLayout]; [self.view layoutIfNeeded]; }];
}

- (BOOL)textField:(UITextField *)textField shouldChangeCharactersInRange:(NSRange)range replacementString:(NSString *)string
{
    [[WASession shared] sendChatState:YES toJID:self.jid];
    return YES;
}

- (void)textFieldDidEndEditing:(UITextField *)textField
{
    [[WASession shared] sendChatState:NO toJID:self.jid];
}

- (BOOL)textFieldShouldReturn:(UITextField *)tf { [self sendTapped]; return NO; }

- (void)sendTapped
{
    NSString *text = [self.field.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (!text.length) return;
    self.field.text = @"";
    self.send.hidden = YES;
    self.micBtn.hidden = NO;
    [[WASession shared] sendChatState:NO toJID:self.jid];

    NSString *mid = [NSString stringWithFormat:@"3EB0%08X%08X", (unsigned int)arc4random(), (unsigned int)arc4random()];
    NSDictionary *quoted = nil;
    if (self.replyingToMsg) {
        NSString *author = [self.replyingToMsg[@"out"] boolValue] ? WALoc(@"you") : (self.replyingToMsg[@"sender"] ?: self.title);
        quoted = @{ @"id": self.replyingToMsg[@"id"] ?: @"",
                    @"text": self.replyingToMsg[@"display"] ?: @"",
                    @"sender": author };
        [self cancelReply];
    }

    [[WAChatStore shared] recordOutgoing:text toJID:self.jid msgId:mid status:WAMessageStatusPending quoted:quoted];
    [[WASession shared] sendText:text toJID:self.jid msgId:mid];
    AudioServicesPlaySystemSound(1004);
    WALog(@"send to %@ [%@]: %lu chars", self.jid, mid, (unsigned long)text.length);
}

- (void)scrollToBottom
{
    NSArray *active = [self activeMessages];
    if (active.count == 0) return;
    NSIndexPath *ip = [NSIndexPath indexPathForRow:active.count - 1 inSection:0];
    [self.table scrollToRowAtIndexPath:ip atScrollPosition:UITableViewScrollPositionBottom animated:YES];
}

- (void)voiceCall
{
    [[WASession shared] placeCallTo:self.jid video:NO];
    WACallViewController *c = [[WACallViewController alloc] initWithJID:self.jid name:self.title video:NO incoming:NO];
    c.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
    [self presentViewController:c animated:YES completion:nil];
}

- (void)videoCall
{
    [[WASession shared] placeCallTo:self.jid video:YES];
    WACallViewController *c = [[WACallViewController alloc] initWithJID:self.jid name:self.title video:YES incoming:NO];
    c.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
    [self presentViewController:c animated:YES completion:nil];
}

#pragma mark - Table & Bubble Rendering

- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return [self activeMessages].count; }

- (NSAttributedString *)attributedTextForMessage:(NSDictionary *)m
{
    NSString *display = ([m[@"kind"] intValue] == WA_MSG_AUDIO) ? [self displayTextFor:m] : m[@"display"];
    if (!display.length) return nil;
    return WAParseMarkdownText(display, [UIFont systemFontOfSize:16], [UIColor colorWithWhite:0.1 alpha:1.0]);
}

- (CGFloat)tableView:(UITableView *)t heightForRowAtIndexPath:(NSIndexPath *)ip
{
    NSDictionary *m = [self activeMessages][ip.row];
    if (m[@"sep"]) return 34;
    CGFloat maxW = self.view.bounds.size.width * 0.72 - 24;
    CGSize thumb = [self thumbSizeFor:m];
    NSAttributedString *as = [self attributedTextForMessage:m];
    CGSize ts = WAMeasureAttributedString(as, maxW);
    CGFloat senderH = [m[@"sender"] length] ? 16 : 0;
    CGFloat quoteH = m[@"quoted"] ? 38 : 0;
    CGFloat reactH = m[@"reaction"] ? 14 : 0;
    CGFloat contentH = senderH + quoteH + thumb.height + (thumb.height > 0 && ts.height > 0 ? 6 : 0) + ts.height + reactH;
    return MAX(contentH, 18) + 14 + 16;
}

- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip
{
    UITableViewCell *cell = [t dequeueReusableCellWithIdentifier:@"b"];
    if (!cell) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"b"];
        cell.backgroundColor = [UIColor clearColor];
        cell.contentView.backgroundColor = [UIColor clearColor];
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
    }
    for (UIView *v in cell.contentView.subviews) [v removeFromSuperview];
    NSDictionary *m = [self activeMessages][ip.row];
    if (m[@"sep"]) {
        NSString *day = m[@"sep"];
        UIFont *df = [UIFont boldSystemFontOfSize:12];
        CGSize ds = [day sizeWithFont:df];
        CGFloat cw = ds.width + 22, cx = (self.view.bounds.size.width - cw) / 2;
        UILabel *chip = [[UILabel alloc] initWithFrame:CGRectMake(cx, 6, cw, 22)];
        chip.text = day;
        chip.font = df;
        chip.textAlignment = NSTextAlignmentCenter;
        chip.textColor = [UIColor colorWithRed:0.2 green:0.35 blue:0.3 alpha:1.0];
        chip.backgroundColor = [UIColor colorWithRed:0.86 green:0.91 blue:0.84 alpha:0.95];
        chip.layer.cornerRadius = 11;
        chip.clipsToBounds = YES;
        [cell.contentView addSubview:chip];
        return cell;
    }

    BOOL out = [m[@"out"] boolValue];
    CGFloat w = self.view.bounds.size.width, maxW = w * 0.72 - 24;
    NSAttributedString *as = [self attributedTextForMessage:m];
    CGSize ts = WAMeasureAttributedString(as, maxW);

    NSString *sender = m[@"sender"];
    CGFloat senderH = sender.length ? 16 : 0;
    CGFloat quoteH = m[@"quoted"] ? 38 : 0;
    CGFloat reactH = m[@"reaction"] ? 14 : 0;
    CGSize thumb = [self thumbSizeFor:m];
    CGFloat gap = (thumb.height > 0 && ts.height > 0) ? 6 : 0;

    NSString *timeStr = WAClockTime([m[@"ts"] unsignedLongLongValue]);
    UIFont *timeFont = [UIFont systemFontOfSize:11];
    CGFloat timeW = timeStr.length ? [timeStr sizeWithFont:timeFont].width : 0;

    NSString *statusGlyph = @"";
    UIColor *statusColor = nil;
    if (out) {
        int st = [m[@"status"] intValue];
        switch (st) {
            case WAMessageStatusError:
                statusGlyph = @" ⚠️";
                statusColor = [UIColor colorWithRed:0.85 green:0.15 blue:0.15 alpha:1.0];
                break;
            case WAMessageStatusPending:
                statusGlyph = @" 🕓";
                statusColor = [UIColor colorWithRed:0.35 green:0.55 blue:0.3 alpha:0.8];
                break;
            case WAMessageStatusSent:
                statusGlyph = @" ✓";
                statusColor = [UIColor colorWithRed:0.25 green:0.5 blue:0.2 alpha:1.0];
                break;
            case WAMessageStatusDelivered:
                statusGlyph = @" ✓✓";
                statusColor = [UIColor colorWithRed:0.25 green:0.5 blue:0.2 alpha:1.0];
                break;
            case WAMessageStatusRead:
            case WAMessageStatusPlayed:
                statusGlyph = @" ✓✓";
                statusColor = [UIColor colorWithRed:0.20 green:0.60 blue:0.86 alpha:1.0];
                break;
            default:
                statusGlyph = @" ✓✓";
                statusColor = [UIColor colorWithRed:0.25 green:0.5 blue:0.2 alpha:1.0];
                break;
        }
    }
    UIFont *statusFont = [UIFont systemFontOfSize:11];
    CGFloat statusW = statusGlyph.length ? [statusGlyph sizeWithFont:statusFont].width : 0;
    BOOL isStarred = [m[@"starred"] boolValue];
    NSString *starGlyph = isStarred ? @"★ " : @"";
    CGFloat starW = starGlyph.length ? [starGlyph sizeWithFont:timeFont].width : 0;
    CGFloat bottomRowW = timeW + statusW + starW;
    CGFloat senderW = sender.length ? [sender sizeWithFont:[UIFont boldSystemFontOfSize:13]].width : 0;

    CGFloat contentW = MAX(MAX(MAX(thumb.width, ts.width), bottomRowW), senderW);
    if (m[@"quoted"]) contentW = MAX(contentW, 140);
    CGFloat contentH = senderH + quoteH + thumb.height + gap + ts.height + reactH;
    CGFloat bw = MIN(contentW + 24, w * 0.78), bh = contentH + 14 + 14;
    CGRect bf = out ? CGRectMake(w - bw - 8, 5, bw, bh) : CGRectMake(8, 5, bw, bh);

    UIImageView *bubble = [[UIImageView alloc] initWithImage:[WATheme bubbleOutgoing:out]];
    bubble.frame = bf;
    [cell.contentView addSubview:bubble];

    CGFloat y = bf.origin.y + 7;
    if (sender.length) {
        UILabel *sl = [[UILabel alloc] initWithFrame:CGRectMake(bf.origin.x + 12, y, bw - 24, 15)];
        sl.font = [UIFont boldSystemFontOfSize:13];
        sl.backgroundColor = [UIColor clearColor];
        sl.textColor = [WATheme accent];
        sl.text = sender;
        [cell.contentView addSubview:sl];
        y += senderH;
    }

    // Quoted message inside bubble
    if (m[@"quoted"]) {
        NSDictionary *q = m[@"quoted"];
        CGFloat qw = bw - 24;
        UIView *qbox = [[UIView alloc] initWithFrame:CGRectMake(bf.origin.x + 12, y, qw, 34)];
        qbox.backgroundColor = [UIColor colorWithWhite:0 alpha:0.06];
        qbox.layer.cornerRadius = 4;
        qbox.clipsToBounds = YES;

        UIView *line = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 3, 34)];
        line.backgroundColor = [WATheme accent];
        [qbox addSubview:line];

        UILabel *qAuthor = [[UILabel alloc] initWithFrame:CGRectMake(8, 2, qw - 12, 14)];
        qAuthor.font = [UIFont boldSystemFontOfSize:11];
        qAuthor.textColor = [WATheme accent];
        qAuthor.backgroundColor = [UIColor clearColor];
        qAuthor.text = q[@"sender"] ?: WALoc(@"you");
        [qbox addSubview:qAuthor];

        UILabel *qText = [[UILabel alloc] initWithFrame:CGRectMake(8, 16, qw - 12, 16)];
        qText.font = [UIFont systemFontOfSize:11];
        qText.textColor = [UIColor colorWithWhite:0.35 alpha:1.0];
        qText.backgroundColor = [UIColor clearColor];
        qText.text = q[@"text"] ?: @"";
        [qbox addSubview:qText];

        [cell.contentView addSubview:qbox];
        y += 34 + 4;
    }

    UIImage *img = m[@"image"];
    if (thumb.height > 0 && img) {
        UIImageView *iv = [[UIImageView alloc] initWithImage:img];
        iv.frame = CGRectMake(bf.origin.x + 12, y, thumb.width, thumb.height);
        iv.contentMode = UIViewContentModeScaleAspectFill;
        iv.clipsToBounds = YES;
        iv.layer.cornerRadius = 5;
        [cell.contentView addSubview:iv];
        if ([m[@"kind"] intValue] == WA_MSG_VIDEO) {
            UILabel *play = [[UILabel alloc] initWithFrame:iv.frame];
            play.text = @"▶";
            play.font = [UIFont systemFontOfSize:40];
            play.textColor = [UIColor colorWithWhite:1.0 alpha:0.9];
            play.textAlignment = NSTextAlignmentCenter;
            play.backgroundColor = [UIColor clearColor];
            [cell.contentView addSubview:play];
        }
        y += thumb.height + gap;
    }

    if (ts.height > 0 && as) {
        UILabel *lbl = [[UILabel alloc] initWithFrame:CGRectMake(bf.origin.x + 12, y, ts.width + 2, ts.height)];
        lbl.numberOfLines = 0;
        lbl.backgroundColor = [UIColor clearColor];
        lbl.attributedText = as;
        [cell.contentView addSubview:lbl];
    }

    CGFloat curX = bf.origin.x + bw - 12 - bottomRowW;
    if (isStarred) {
        UILabel *starLbl = [[UILabel alloc] initWithFrame:CGRectMake(curX, bf.origin.y + bh - 16, starW + 1, 13)];
        starLbl.font = timeFont;
        starLbl.backgroundColor = [UIColor clearColor];
        starLbl.textColor = [UIColor colorWithRed:0.95 green:0.75 blue:0.1 alpha:1.0];
        starLbl.text = starGlyph;
        [cell.contentView addSubview:starLbl];
        curX += starW;
    }
    if (timeStr.length) {
        UILabel *tl = [[UILabel alloc] initWithFrame:CGRectMake(curX, bf.origin.y + bh - 16, timeW + 1, 13)];
        tl.font = timeFont;
        tl.backgroundColor = [UIColor clearColor];
        tl.textColor = out ? [UIColor colorWithRed:0.25 green:0.5 blue:0.2 alpha:1.0] : [UIColor colorWithWhite:0.55 alpha:1.0];
        tl.text = timeStr;
        [cell.contentView addSubview:tl];
        curX += timeW;
    }
    if (statusGlyph.length) {
        UILabel *stl = [[UILabel alloc] initWithFrame:CGRectMake(curX, bf.origin.y + bh - 16, statusW + 1, 13)];
        stl.font = statusFont;
        stl.backgroundColor = [UIColor clearColor];
        stl.textColor = statusColor;
        stl.text = statusGlyph;
        [cell.contentView addSubview:stl];
    }

    // Reaction badge
    if (m[@"reaction"]) {
        NSString *rx = m[@"reaction"];
        UILabel *rxLbl = [[UILabel alloc] init];
        rxLbl.text = rx;
        rxLbl.font = [UIFont systemFontOfSize:14];
        rxLbl.textAlignment = NSTextAlignmentCenter;
        rxLbl.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.95];
        rxLbl.layer.cornerRadius = 9;
        rxLbl.layer.borderWidth = 1.0;
        rxLbl.layer.borderColor = [UIColor colorWithWhite:0.85 alpha:1.0].CGColor;
        rxLbl.clipsToBounds = YES;
        rxLbl.frame = CGRectMake(out ? (bf.origin.x + 8) : (bf.origin.x + bw - 32), bf.origin.y + bh - 10, 26, 20);
        [cell.contentView addSubview:rxLbl];
    }

    return cell;
}

- (void)openContactInfo
{
    WAContactInfoViewController *info = [[WAContactInfoViewController alloc] initWithJID:self.jid name:self.title];
    [self.navigationController pushViewController:info animated:YES];
}

#pragma mark - Long Press & Context Menu

- (void)onLongPress:(UILongPressGestureRecognizer *)g
{
    if (g.state != UIGestureRecognizerStateBegan) return;
    NSIndexPath *ip = [self.table indexPathForRowAtPoint:[g locationInView:self.table]];
    if (!ip) return;
    NSDictionary *m = [self activeMessages][ip.row];
    if (m[@"sep"]) return;

    self.selectedMsg = m;
    NSString *preview = m[@"display"];
    if (preview.length > 30) preview = [[preview substringToIndex:30] stringByAppendingString:@"..."];

    BOOL isStarred = [m[@"starred"] boolValue];
    NSString *starTitle = isStarred ? WALoc(@"unstar_message") : WALoc(@"star_message");

    UIActionSheet *as = [[UIActionSheet alloc] initWithTitle:(preview.length ? preview : nil)
                                                    delegate:self
                                           cancelButtonTitle:WALoc(@"cancel")
                                      destructiveButtonTitle:WALoc(@"delete_message")
                                           otherButtonTitles:WALoc(@"reply"), WALoc(@"react"), WALoc(@"forward"), WALoc(@"copy"), starTitle, nil];
    as.tag = 200;
    if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
        UITableViewCell *cell = [self.table cellForRowAtIndexPath:ip];
        [as showFromRect:(cell ? cell.bounds : CGRectMake(0, 0, 100, 50)) inView:(cell ?: self.view) animated:YES];
    } else {
        [as showInView:self.view];
    }
}

- (void)actionSheet:(UIActionSheet *)actionSheet clickedButtonAtIndex:(NSInteger)buttonIndex
{
    if (actionSheet.tag == 200) {
        if (buttonIndex == actionSheet.destructiveButtonIndex) {
            if (self.selectedMsg) {
                [[WAChatStore shared] deleteMessage:self.selectedMsg inChat:self.jid];
                self.selectedMsg = nil;
            }
        } else if (buttonIndex == 1) { // Reply
            if (self.selectedMsg) {
                self.replyingToMsg = self.selectedMsg;
                NSString *author = [self.replyingToMsg[@"out"] boolValue] ? WALoc(@"you") : (self.replyingToMsg[@"sender"] ?: self.title);
                self.replyAuthorLabel.text = author;
                self.replyTextLabel.text = self.replyingToMsg[@"display"];
                self.replyBar.hidden = NO;
                [self.field becomeFirstResponder];
                [self.view setNeedsLayout];
                self.selectedMsg = nil;
            }
        } else if (buttonIndex == 2) { // React
            UIActionSheet *rxSheet = [[UIActionSheet alloc] initWithTitle:WALoc(@"react")
                                                                 delegate:self
                                                        cancelButtonTitle:WALoc(@"cancel")
                                                   destructiveButtonTitle:nil
                                                        otherButtonTitles:@"👍", @"❤️", @"😂", @"😮", @"😢", @"🙏", WALoc(@"remove_reaction"), nil];
            rxSheet.tag = 201;
            if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
                [rxSheet showFromRect:self.table.bounds inView:self.table animated:YES];
            } else {
                [rxSheet showInView:self.view];
            }
        } else if (buttonIndex == 3) { // Forward
            NSArray *convs = [[WAChatStore shared] conversationsSorted];
            NSMutableArray *others = [NSMutableArray array];
            for (NSDictionary *c in convs) {
                if (![c[@"jid"] isEqualToString:self.jid]) [others addObject:c];
            }
            self.forwardCandidates = others;
            if (others.count == 0) {
                UIAlertView *av = [[UIAlertView alloc] initWithTitle:nil message:WALoc(@"no_chats") delegate:nil cancelButtonTitle:WALoc(@"ok") otherButtonTitles:nil];
                [av show];
                return;
            }
            UIActionSheet *fwdSheet = [[UIActionSheet alloc] initWithTitle:WALoc(@"forward_to")
                                                                  delegate:self
                                                         cancelButtonTitle:WALoc(@"cancel")
                                                    destructiveButtonTitle:nil
                                                         otherButtonTitles:nil];
            for (NSDictionary *c in others) [fwdSheet addButtonWithTitle:c[@"name"]];
            fwdSheet.tag = 202;
            if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
                [fwdSheet showFromRect:self.table.bounds inView:self.table animated:YES];
            } else {
                [fwdSheet showInView:self.view];
            }
        } else if (buttonIndex == 4) { // Copy
            NSString *text = self.selectedMsg[@"display"];
            if (text.length) {
                [[UIPasteboard generalPasteboard] setString:text];
                UIAlertView *a = [[UIAlertView alloc] initWithTitle:nil message:WALoc(@"copied") delegate:nil cancelButtonTitle:nil otherButtonTitles:nil];
                [a show];
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.7 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [a dismissWithClickedButtonIndex:0 animated:YES]; });
            }
            self.selectedMsg = nil;
        } else if (buttonIndex == 5) { // Star / Unstar
            if (self.selectedMsg) {
                [[WAChatStore shared] toggleStarMessage:self.selectedMsg inChat:self.jid];
                self.selectedMsg = nil;
            }
        }
    } else if (actionSheet.tag == 201) { // Reaction emojis
        if (!self.selectedMsg) return;
        NSString *mid = self.selectedMsg[@"id"];
        NSArray *emojis = @[ @"👍", @"❤️", @"😂", @"😮", @"😢", @"🙏", @"" ];
        if (buttonIndex < emojis.count) {
            NSString *chosen = emojis[buttonIndex];
            [[WAChatStore shared] addReaction:chosen toMessageId:mid inChat:self.jid];
        }
        self.selectedMsg = nil;
    } else if (actionSheet.tag == 202) { // Forward target selection
        if (buttonIndex < self.forwardCandidates.count && self.selectedMsg) {
            NSDictionary *target = self.forwardCandidates[buttonIndex];
            NSString *targetJID = target[@"jid"];
            NSString *text = self.selectedMsg[@"text"] ?: self.selectedMsg[@"display"];
            NSData *thumb = self.selectedMsg[@"thumb"];
            NSString *mid = [NSString stringWithFormat:@"3EB0%08X%08X", (unsigned int)arc4random(), (unsigned int)arc4random()];

            if (thumb.length) {
                [[WAChatStore shared] recordOutgoingMedia:thumb caption:(text ?: @"") toJID:targetJID msgId:mid status:WAMessageStatusPending];
                // Reuse image payload or send text caption
                [[WASession shared] sendText:(text ?: @"📷") toJID:targetJID msgId:mid];
            } else if (text.length) {
                [[WAChatStore shared] recordOutgoing:text toJID:targetJID msgId:mid status:WAMessageStatusPending];
                [[WASession shared] sendText:text toJID:targetJID msgId:mid];
            }
            UIAlertView *av = [[UIAlertView alloc] initWithTitle:nil message:WALoc(@"forwarded") delegate:nil cancelButtonTitle:nil otherButtonTitles:nil];
            [av show];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.8 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [av dismissWithClickedButtonIndex:0 animated:YES]; });
        }
        self.selectedMsg = nil;
        self.forwardCandidates = nil;
    } else if (actionSheet.tag == 300) { // Attach options
        if (buttonIndex == 0) { // Take photo
            if ([UIImagePickerController isSourceTypeAvailable:UIImagePickerControllerSourceTypeCamera]) {
                UIImagePickerController *picker = [[UIImagePickerController alloc] init];
                picker.sourceType = UIImagePickerControllerSourceTypeCamera;
                picker.delegate = self;
                [self presentViewController:picker animated:YES completion:nil];
            } else {
                UIAlertView *av = [[UIAlertView alloc] initWithTitle:nil message:WALoc(@"camera_unavailable") delegate:nil cancelButtonTitle:WALoc(@"ok") otherButtonTitles:nil];
                [av show];
            }
        } else if (buttonIndex == 1) { // Choose photo from library
            if ([UIImagePickerController isSourceTypeAvailable:UIImagePickerControllerSourceTypePhotoLibrary]) {
                UIImagePickerController *picker = [[UIImagePickerController alloc] init];
                picker.sourceType = UIImagePickerControllerSourceTypePhotoLibrary;
                picker.delegate = self;
                if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
                    self.popover = [[UIPopoverController alloc] initWithContentViewController:picker];
                    [self.popover presentPopoverFromRect:self.attach.bounds inView:self.attach permittedArrowDirections:UIPopoverArrowDirectionAny animated:YES];
                } else {
                    [self presentViewController:picker animated:YES completion:nil];
                }
            }
        }
    }
}

#pragma mark - Table Selection & Media

- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)ip
{
    [t deselectRowAtIndexPath:ip animated:NO];
    NSDictionary *m = [self activeMessages][ip.row];
    int kind = [m[@"kind"] intValue];
    if ((kind == WA_MSG_IMAGE || kind == WA_MSG_VIDEO) && (m[@"image"] || m[@"thumb"] || m[@"dpath"])) {
        WAMediaViewController *v = [[WAMediaViewController alloc] initWithMessage:m];
        v.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
        [self presentViewController:v animated:YES completion:nil];
        return;
    }
    if (kind == WA_MSG_AUDIO) {
        [[WAAudioPlayer shared] playOrToggleMessage:m];
        return;
    }
    if (kind == WA_MSG_DOCUMENT) {
        [self openDocumentMessage:m];
        return;
    }
    if ([m[@"out"] boolValue] && [m[@"status"] intValue] == WAMessageStatusError) {
        UIAlertView *alert = [[UIAlertView alloc] initWithTitle:WALoc(@"send_failed_title")
                                                        message:WALoc(@"send_failed_hint")
                                                       delegate:nil
                                              cancelButtonTitle:WALoc(@"ok")
                                              otherButtonTitles:nil];
        [alert show];
        return;
    }
}

- (void)openDocumentMessage:(NSDictionary *)m
{
    NSString *mid = m[@"id"] ?: [NSString stringWithFormat:@"%llu", [m[@"ts"] unsignedLongLongValue]];
    NSString *fn = m[@"file"];
    if (!fn.length) fn = @"document.pdf";
    NSString *docPath = [WADocumentsPath() stringByAppendingPathComponent:[NSString stringWithFormat:@"wa6_doc_%@_%@", mid, fn]];
    if ([[NSFileManager defaultManager] fileExistsAtPath:docPath]) {
        [self showDocumentAtURL:[NSURL fileURLWithPath:docPath]];
        return;
    }
    NSString *dpath = m[@"dpath"];
    NSData *mkey = m[@"mkey"];
    NSData *esha = m[@"esha"];
    if (![dpath isKindOfClass:[NSString class]] || ![mkey isKindOfClass:[NSData class]] || mkey.length != 32) {
        return;
    }
    UIAlertView *av = [[UIAlertView alloc] initWithTitle:nil message:WALoc(@"downloading") delegate:nil cancelButtonTitle:nil otherButtonTitles:nil];
    [av show];
    const char *cdpath = strdup(dpath.UTF8String);
    NSData *mk = mkey, *es = [esha isKindOfClass:[NSData class]] ? esha : nil;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        wa_buf out;
        wa_buf_init(&out);
        int rc = wa_media_download(cdpath, mk.bytes, es.length ? es.bytes : NULL, es.length, WA_MSG_DOCUMENT, NULL, &out);
        free((void *)cdpath);
        if (rc == 0 && out.len > 0) {
            NSData *data = [NSData dataWithBytes:out.data length:out.len];
            wa_buf_free(&out);
            [data writeToFile:docPath atomically:YES];
            dispatch_async(dispatch_get_main_queue(), ^{
                [av dismissWithClickedButtonIndex:0 animated:YES];
                [self showDocumentAtURL:[NSURL fileURLWithPath:docPath]];
            });
        } else {
            wa_buf_free(&out);
            dispatch_async(dispatch_get_main_queue(), ^{
                [av dismissWithClickedButtonIndex:0 animated:YES];
                UIAlertView *err = [[UIAlertView alloc] initWithTitle:nil message:WALoc(@"download_failed") delegate:nil cancelButtonTitle:WALoc(@"ok") otherButtonTitles:nil];
                [err show];
            });
        }
    });
}

- (void)showDocumentAtURL:(NSURL *)url
{
    self.docController = [UIDocumentInteractionController interactionControllerWithURL:url];
    self.docController.delegate = self;
    if (![self.docController presentPreviewAnimated:YES]) {
        [self.docController presentOpenInMenuFromRect:self.view.bounds inView:self.view animated:YES];
    }
}

- (UIViewController *)documentInteractionControllerViewControllerForPreview:(UIDocumentInteractionController *)controller
{
    return self;
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

@end
