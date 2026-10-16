#import "WAChatViewController.h"
#import "WACallViewController.h"
#import "WAMediaViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WASession.h"
#import "../WAChatStore.h"
#import "../WAAudioPlayer.h"
#import <QuartzCore/QuartzCore.h>
#include "wa_history.h"   // WA_MSG_* kinds

@interface WAChatViewController () <UIDocumentInteractionControllerDelegate>
@property (nonatomic, copy) NSString *jid;
@property (nonatomic) BOOL isGroup;
@property (nonatomic, strong) NSMutableArray *messages;   // @{ @"out", @"kind", @"display", @"image"(optional), @"sender" }
@property (nonatomic, strong) UITableView *table;
@property (nonatomic, strong) UIView *bar;
@property (nonatomic, strong) UITextField *field;
@property (nonatomic, strong) UIButton *send;
@property (nonatomic) CGFloat kbHeight;
@property (nonatomic, strong) UIDocumentInteractionController *docController;
@property (nonatomic, strong) UILabel *subtitleLabel;
@end

@implementation WAChatViewController

- (instancetype)initWithJID:(NSString *)jid name:(NSString *)name
{
    if ((self = [super init])) {
        _jid = [jid copy];
        _isGroup = [jid rangeOfString:@"@g.us"].location != NSNotFound;
        self.title = name;
        _messages = [NSMutableArray array];
    }
    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [[UIColor alloc] initWithPatternImage:[WATheme wallpaper]];

    UIButton *call = [UIButton buttonWithType:UIButtonTypeCustom];
    [call setImage:[WATheme roundCallButton:[WATheme accent] diameter:30 glyph:@"☎"] forState:UIControlStateNormal];
    call.frame = CGRectMake(0, 0, 30, 30);
    [call addTarget:self action:@selector(voiceCall) forControlEvents:UIControlEventTouchUpInside];
    UIButton *video = [UIButton buttonWithType:UIButtonTypeCustom];
    [video setImage:[WATheme roundCallButton:[WATheme accent] diameter:30 glyph:@"▶"] forState:UIControlStateNormal];
    video.frame = CGRectMake(0, 0, 30, 30);
    [video addTarget:self action:@selector(videoCall) forControlEvents:UIControlEventTouchUpInside];
    self.navigationItem.rightBarButtonItems = @[ [[UIBarButtonItem alloc] initWithCustomView:video],
                                                 [[UIBarButtonItem alloc] initWithCustomView:call] ];

    UIView *tv = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 180, 44)];
    UILabel *titleLbl = [[UILabel alloc] initWithFrame:CGRectMake(0, 4, 180, 20)];
    titleLbl.text = self.title;
    titleLbl.font = [UIFont boldSystemFontOfSize:17];
    titleLbl.textColor = [UIColor whiteColor];
    titleLbl.textAlignment = NSTextAlignmentCenter;
    titleLbl.backgroundColor = [UIColor clearColor];
    [tv addSubview:titleLbl];

    self.subtitleLabel = [[UILabel alloc] initWithFrame:CGRectMake(0, 24, 180, 16)];
    self.subtitleLabel.font = [UIFont systemFontOfSize:11];
    self.subtitleLabel.textColor = [UIColor colorWithWhite:0.92 alpha:1.0];
    self.subtitleLabel.textAlignment = NSTextAlignmentCenter;
    self.subtitleLabel.backgroundColor = [UIColor clearColor];
    [tv addSubview:self.subtitleLabel];
    self.navigationItem.titleView = tv;

    self.table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStylePlain];
    self.table.backgroundColor = [UIColor clearColor];
    self.table.separatorStyle = UITableViewCellSeparatorStyleNone;
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.allowsSelection = YES;   // tapping an image row opens the full-screen viewer
    [self.table addGestureRecognizer:[[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(onLongPress:)]];
    [self.view addSubview:self.table];

    self.bar = [[UIView alloc] init];
    self.bar.backgroundColor = [UIColor colorWithWhite:0.93 alpha:1.0];
    [self.view addSubview:self.bar];

    self.field = [[UITextField alloc] init];
    self.field.borderStyle = UITextBorderStyleRoundedRect;
    self.field.placeholder = WALoc(@"message_hint");
    self.field.font = [UIFont systemFontOfSize:16];
    self.field.returnKeyType = UIReturnKeySend;
    self.field.delegate = self;
    [self.bar addSubview:self.field];

    self.send = [UIButton buttonWithType:UIButtonTypeCustom];
    [self.send setTitle:WALoc(@"send") forState:UIControlStateNormal];
    [self.send setBackgroundImage:[WATheme glossyButtonWithColor:[WATheme accent]] forState:UIControlStateNormal];
    self.send.titleLabel.font = [UIFont boldSystemFontOfSize:15];
    [self.send addTarget:self action:@selector(sendTapped) forControlEvents:UIControlEventTouchUpInside];
    [self.bar addSubview:self.send];

    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(kbShow:) name:UIKeyboardWillShowNotification object:nil];
    [nc addObserver:self selector:@selector(kbHide:) name:UIKeyboardWillHideNotification object:nil];
    [nc addObserver:self selector:@selector(onStoreChanged:) name:WAChatStoreChanged object:nil];
    [nc addObserver:self selector:@selector(onAudioPlayerState:) name:WAAudioPlayerStateChanged object:nil];
    [nc addObserver:self selector:@selector(onPresence:) name:WASessionPresenceChanged object:nil];

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

- (void)viewWillDisappear:(BOOL)animated
{
    [super viewWillDisappear:animated];
    [[WAAudioPlayer shared] stop];
    [[WASession shared] unsubscribePresence:self.jid];
    [[WASession shared] sendChatState:NO toJID:self.jid];
}

- (void)onPresence:(NSNotification *)n
{
    NSString *fromJid = n.userInfo[@"jid"];
    if (!fromJid.length || [fromJid rangeOfString:jidKey(self.jid)].location == NSNotFound) return;
    NSString *st = n.userInfo[@"state"];
    if ([st isEqualToString:@"composing"]) {
        self.subtitleLabel.text = WALoc(@"typing");
    } else if ([st isEqualToString:@"available"]) {
        self.subtitleLabel.text = WALoc(@"online");
    } else if ([st isEqualToString:@"unavailable"]) {
        NSString *last = n.userInfo[@"last"];
        if (last.length) {
            self.subtitleLabel.text = [NSString stringWithFormat:@"%@ %@", WALoc(@"last_seen"), WAClockTime((unsigned long long)[last longLongValue])];
        } else {
            self.subtitleLabel.text = @"";
        }
    } else if ([st isEqualToString:@"paused"]) {
        self.subtitleLabel.text = WALoc(@"online");
    }
}

- (void)onAudioPlayerState:(NSNotification *)n
{
    [self.table reloadData];
}

// the digits of a JID's user part (device suffix / server stripped), matching WAChatStore's canonical key
static NSString *jidKey(NSString *jid)
{
    NSString *user = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
    NSString *d = [[user componentsSeparatedByCharactersInSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]] componentsJoinedByString:@""];
    return d.length ? d : jid;
}

static NSString *durationStr(unsigned int secs)
{
    return [NSString stringWithFormat:@"%u:%02u", secs / 60, secs % 60];
}

// The text shown in a bubble: the message text, or a media label (+ filename / duration / caption) for media.
- (NSString *)displayTextFor:(NSDictionary *)m
{
    int kind = [m[@"kind"] intValue];
    NSString *caption = m[@"text"];
    if (kind == WA_MSG_TEXT) return caption.length ? caption : @"";

    if (kind == WA_MSG_AUDIO) {
        WAAudioPlayer *ap = [WAAudioPlayer shared];
        BOOL isThis = [ap.currentMsgId length] && [ap.currentMsgId isEqualToString:m[@"id"]];
        NSMutableString *s = [NSMutableString string];
        if (isThis && ap.isPlaying) {
            [s appendFormat:@"⏸ %@ · %@ / %@", WALoc(@"media_voice"), durationStr((unsigned int)ap.currentTime), durationStr([m[@"dur"] unsignedIntValue])];
        } else if (isThis && ap.isPaused) {
            [s appendFormat:@"▶️ %@ · %@ / %@", WALoc(@"media_voice"), durationStr((unsigned int)ap.currentTime), durationStr([m[@"dur"] unsignedIntValue])];
        } else {
            [s appendFormat:@"▶️ %@", WALoc(@"media_voice")];
            if ([m[@"dur"] unsignedIntValue] > 0)
                [s appendFormat:@" · %@", durationStr([m[@"dur"] unsignedIntValue])];
        }
        if (caption.length) [s appendFormat:@"\n%@", caption];
        return s;
    }

    NSMutableString *s = [NSMutableString string];
    if (kind == WA_MSG_DOCUMENT && [m[@"file"] length]) [s appendFormat:@"\U0001F4C4 %@", m[@"file"]];
    else [s appendString:[WAChatStore mediaLabelForKind:kind]];
    if (kind == WA_MSG_VIDEO && [m[@"dur"] unsignedIntValue] > 0)
        [s appendFormat:@" · %@", durationStr([m[@"dur"] unsignedIntValue])];
    if (caption.length) [s appendFormat:@"\n%@", caption];
    return s;
}

// Load this chat's history (and any live messages) from the store: decode thumbnails and precompute the bubble text.
- (void)loadMessages
{
    [self.messages removeAllObjects];
    NSCalendar *cal = [NSCalendar currentCalendar];
    NSInteger lastDay = NSIntegerMin;
    for (NSDictionary *m in [[WAChatStore shared] messagesForJID:self.jid]) {
        unsigned long long mts = [m[@"ts"] unsignedLongLongValue];
        if (mts) {
            NSInteger day = [cal ordinalityOfUnit:NSDayCalendarUnit inUnit:NSEraCalendarUnit
                                          forDate:[NSDate dateWithTimeIntervalSince1970:mts]];
            if (day != lastDay) { [self.messages addObject:@{ @"sep": WADayLabel(mts) }]; lastDay = day; }
        }
        NSMutableDictionary *d = [NSMutableDictionary dictionary];
        d[@"out"] = m[@"out"] ?: @NO;
        d[@"kind"] = m[@"kind"] ?: @(WA_MSG_TEXT);
        d[@"ts"] = m[@"ts"] ?: @0;
        d[@"status"] = m[@"status"] ?: ([m[@"out"] boolValue] ? @(WAMessageStatusDelivered) : @0);
        if ([m[@"id"] length]) d[@"id"] = m[@"id"];
        d[@"display"] = [self displayTextFor:m];
        if (self.isGroup && ![m[@"out"] boolValue] && [m[@"sender"] length]) d[@"sender"] = m[@"sender"];
        // carry media refs for the full-screen viewer and audio player
        for (NSString *k in @[ @"thumb", @"dpath", @"mkey", @"esha", @"dur", @"file", @"audio_data" ]) if (m[k]) d[k] = m[k];
        int kind = [m[@"kind"] intValue];
        NSData *thumb = m[@"thumb"];
        if ((kind == WA_MSG_IMAGE || kind == WA_MSG_VIDEO) && [thumb isKindOfClass:[NSData class]] && thumb.length) {
            UIImage *img = [UIImage imageWithData:thumb];
            if (img) d[@"image"] = img;
        }
        [self.messages addObject:d];
    }
}

// The on-screen size of a message's thumbnail (CGSizeZero when there is none), scaled into the bubble width.
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
    if (changed.length && ![changed isEqualToString:jidKey(self.jid)]) return;   // a different chat changed
    [self loadMessages];
    [self.table reloadData];
    [self scrollToBottom];
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    CGFloat w = self.view.bounds.size.width, h = self.view.bounds.size.height;
    CGFloat barH = 46;
    self.bar.frame = CGRectMake(0, h - barH - self.kbHeight, w, barH);
    self.field.frame = CGRectMake(8, 7, w - 8 - 70 - 8, barH - 14);
    self.send.frame = CGRectMake(w - 70 - 8, 7, 70, barH - 14);
    self.table.frame = CGRectMake(0, 0, w, h - barH - self.kbHeight);
}

- (void)kbShow:(NSNotification *)n
{
    CGRect r = [n.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    self.kbHeight = [self.view convertRect:r fromView:nil].size.height;
    [UIView animateWithDuration:0.25 animations:^{ [self viewDidLayoutSubviews]; }];
}

- (void)kbHide:(NSNotification *)n
{
    self.kbHeight = 0;
    [UIView animateWithDuration:0.25 animations:^{ [self viewDidLayoutSubviews]; }];
}

- (BOOL)textField:(UITextField *)textField shouldChangeCharactersInRange:(NSRange)range replacementString:(NSString *)string
{
    NSString *newText = [textField.text stringByReplacingCharactersInRange:range withString:string];
    [[WASession shared] sendChatState:(newText.length > 0) toJID:self.jid];
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
    [[WASession shared] sendChatState:NO toJID:self.jid];
    NSString *mid = [NSString stringWithFormat:@"3EB0%08X%08X", (unsigned int)arc4random(), (unsigned int)arc4random()];
    [[WAChatStore shared] recordOutgoing:text toJID:self.jid msgId:mid status:WAMessageStatusPending];
    [[WASession shared] sendText:text toJID:self.jid msgId:mid];
    WALog(@"send to %@ [%@]: %lu chars", self.jid, mid, (unsigned long)text.length);
}

- (void)scrollToBottom
{
    if (self.messages.count == 0) return;
    NSIndexPath *ip = [NSIndexPath indexPathForRow:self.messages.count - 1 inSection:0];
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

#pragma mark - table (bubbles)

- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return self.messages.count; }

// The text's drawn size inside a bubble of the given max content width.
- (CGSize)textSize:(NSString *)text maxWidth:(CGFloat)maxW
{
    if (!text.length) return CGSizeZero;
    return [text sizeWithFont:[UIFont systemFontOfSize:16]
             constrainedToSize:CGSizeMake(maxW, 10000) lineBreakMode:NSLineBreakByWordWrapping];
}

- (CGFloat)tableView:(UITableView *)t heightForRowAtIndexPath:(NSIndexPath *)ip
{
    NSDictionary *m = self.messages[ip.row];
    if (m[@"sep"]) return 34;
    CGFloat maxW = self.view.bounds.size.width * 0.72 - 24;
    CGSize thumb = [self thumbSizeFor:m];
    NSString *display = ([m[@"kind"] intValue] == WA_MSG_AUDIO) ? [self displayTextFor:m] : m[@"display"];
    CGSize ts = [self textSize:display maxWidth:maxW];
    CGFloat senderH = [m[@"sender"] length] ? 16 : 0;
    CGFloat contentH = senderH + thumb.height + (thumb.height > 0 && ts.height > 0 ? 6 : 0) + ts.height;
    return MAX(contentH, 18) + 14 + 16;   // content + time row + vertical padding
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
    NSDictionary *m = self.messages[ip.row];
    if (m[@"sep"]) {   // a centered date chip between days
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
    NSString *text = ([m[@"kind"] intValue] == WA_MSG_AUDIO) ? [self displayTextFor:m] : m[@"display"];
    CGFloat w = self.view.bounds.size.width, maxW = w * 0.72 - 24;

    NSString *sender = m[@"sender"];
    CGFloat senderH = sender.length ? 16 : 0;
    CGSize thumb = [self thumbSizeFor:m];
    CGSize ts = [self textSize:text maxWidth:maxW];
    CGFloat gap = (thumb.height > 0 && ts.height > 0) ? 6 : 0;

    // time (+ sent tick for outgoing) shown small at the bubble's bottom-right
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
    CGFloat bottomRowW = timeW + statusW;
    CGFloat senderW = sender.length ? [sender sizeWithFont:[UIFont boldSystemFontOfSize:13]].width : 0;

    CGFloat contentW = MAX(MAX(MAX(thumb.width, ts.width), bottomRowW), senderW);
    CGFloat contentH = senderH + thumb.height + gap + ts.height;
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
    UIImage *img = m[@"image"];
    if (thumb.height > 0 && img) {
        UIImageView *iv = [[UIImageView alloc] initWithImage:img];
        iv.frame = CGRectMake(bf.origin.x + 12, y, thumb.width, thumb.height);
        iv.contentMode = UIViewContentModeScaleAspectFill;
        iv.clipsToBounds = YES;
        iv.layer.cornerRadius = 5;
        [cell.contentView addSubview:iv];
        if ([m[@"kind"] intValue] == WA_MSG_VIDEO) {   // a play glyph over the video thumbnail
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
    if (ts.height > 0) {
        UILabel *lbl = [[UILabel alloc] initWithFrame:CGRectMake(bf.origin.x + 12, y, ts.width + 2, ts.height)];
        lbl.numberOfLines = 0;
        lbl.backgroundColor = [UIColor clearColor];
        lbl.font = [UIFont systemFontOfSize:16];
        lbl.textColor = [UIColor colorWithWhite:0.1 alpha:1.0];
        lbl.text = text;
        [cell.contentView addSubview:lbl];
    }
    if (timeStr.length) {
        UILabel *tl = [[UILabel alloc] initWithFrame:CGRectMake(bf.origin.x + bw - 12 - bottomRowW, bf.origin.y + bh - 16, timeW + 1, 13)];
        tl.font = timeFont;
        tl.backgroundColor = [UIColor clearColor];
        tl.textColor = out ? [UIColor colorWithRed:0.25 green:0.5 blue:0.2 alpha:1.0] : [UIColor colorWithWhite:0.55 alpha:1.0];
        tl.text = timeStr;
        [cell.contentView addSubview:tl];
    }
    if (statusGlyph.length) {
        UILabel *stl = [[UILabel alloc] initWithFrame:CGRectMake(bf.origin.x + bw - 12 - statusW, bf.origin.y + bh - 16, statusW + 1, 13)];
        stl.font = statusFont;
        stl.backgroundColor = [UIColor clearColor];
        stl.textColor = statusColor;
        stl.text = statusGlyph;
        [cell.contentView addSubview:stl];
    }
    return cell;
}

// Long-press a text bubble to copy it.
- (void)onLongPress:(UILongPressGestureRecognizer *)g
{
    if (g.state != UIGestureRecognizerStateBegan) return;
    NSIndexPath *ip = [self.table indexPathForRowAtPoint:[g locationInView:self.table]];
    if (!ip) return;
    NSDictionary *m = self.messages[ip.row];
    NSString *text = m[@"display"];
    if (m[@"sep"] || !text.length) return;
    [[UIPasteboard generalPasteboard] setString:text];
    UIAlertView *a = [[UIAlertView alloc] initWithTitle:nil message:WALoc(@"copied") delegate:nil cancelButtonTitle:nil otherButtonTitles:nil];
    [a show];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.7 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [a dismissWithClickedButtonIndex:0 animated:YES]; });
}

- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)ip
{
    [t deselectRowAtIndexPath:ip animated:NO];
    NSDictionary *m = self.messages[ip.row];
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
