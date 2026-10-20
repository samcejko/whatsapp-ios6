#import "WAMediaGalleryViewController.h"
#import "WAMediaViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import "../WAChatStore.h"
#import <QuartzCore/QuartzCore.h>
#include "wa_history.h"

@interface WAMediaGalleryViewController ()
@property (nonatomic, strong) NSArray *mediaMessages;
@property (nonatomic, strong) UIScrollView *scrollView;
@property (nonatomic, strong) UILabel *emptyLabel;
@end

@implementation WAMediaGalleryViewController

- (instancetype)initWithMessages:(NSArray *)messages
{
    if ((self = [super init])) {
        NSMutableArray *med = [NSMutableArray array];
        for (NSDictionary *m in messages) {
            int k = [m[@"kind"] intValue];
            if ((k == WA_MSG_IMAGE || k == WA_MSG_VIDEO) && (m[@"thumb"] || m[@"image"] || m[@"dpath"])) {
                [med addObject:m];
            }
        }
        _mediaMessages = med;
    }
    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = WALoc(@"media_shared");
    self.view.backgroundColor = [WATheme chatBackgroundColor];

    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithTitle:WALoc(@"ok") style:UIBarButtonItemStyleDone target:self action:@selector(close)];

    self.scrollView = [[UIScrollView alloc] initWithFrame:self.view.bounds];
    self.scrollView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:self.scrollView];

    self.emptyLabel = [[UILabel alloc] init];
    self.emptyLabel.text = WALoc(@"media_shared");
    self.emptyLabel.textAlignment = NSTextAlignmentCenter;
    self.emptyLabel.textColor = [WATheme secondaryActionColor];
    self.emptyLabel.font = [WATheme bodyFont];
    self.emptyLabel.hidden = (self.mediaMessages.count > 0);
    [self.view addSubview:self.emptyLabel];

    [self populateGrid];
}

- (void)close
{
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)populateGrid
{
    for (UIView *v in self.scrollView.subviews) [v removeFromSuperview];

    CGFloat w = self.view.bounds.size.width;
    CGFloat pad = 4;
    int cols = (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) ? 6 : 4;
    CGFloat itemSize = floorf((w - (cols + 1) * pad) / cols);

    CGFloat x = pad, y = pad;
    for (NSUInteger i = 0; i < self.mediaMessages.count; i++) {
        NSDictionary *m = self.mediaMessages[i];
        UIImage *img = nil;
        if (m[@"image"]) {
            img = m[@"image"];
        } else if (m[@"thumb"] && [m[@"thumb"] isKindOfClass:[NSData class]]) {
            img = [UIImage imageWithData:m[@"thumb"]];
        }
        if (!img) img = [WATheme wallpaper];

        UIButton *btn = [UIButton buttonWithType:UIButtonTypeCustom];
        btn.frame = CGRectMake(x, y, itemSize, itemSize);
        btn.tag = i;
        btn.clipsToBounds = YES;
        btn.layer.cornerRadius = 2;
        [btn setImage:img forState:UIControlStateNormal];
        btn.imageView.contentMode = UIViewContentModeScaleAspectFill;
        [btn addTarget:self action:@selector(thumbnailTapped:) forControlEvents:UIControlEventTouchUpInside];

        if ([m[@"kind"] intValue] == WA_MSG_VIDEO) {
            UILabel *vglyph = [[UILabel alloc] initWithFrame:CGRectMake(4, itemSize - 18, 20, 16)];
            vglyph.text = @"▶";
            vglyph.font = [UIFont boldSystemFontOfSize:12];
            vglyph.textColor = [UIColor whiteColor];
            vglyph.backgroundColor = [UIColor colorWithWhite:0 alpha:0.5];
            vglyph.textAlignment = NSTextAlignmentCenter;
            [btn addSubview:vglyph];
        }

        [self.scrollView addSubview:btn];

        if ((i + 1) % cols == 0) {
            x = pad;
            y += itemSize + pad;
        } else {
            x += itemSize + pad;
        }
    }

    if (self.mediaMessages.count % cols != 0) {
        y += itemSize + pad;
    }
    self.scrollView.contentSize = CGSizeMake(w, y + pad);
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    self.emptyLabel.frame = CGRectMake(20, self.view.bounds.size.height / 2 - 20, self.view.bounds.size.width - 40, 40);
    [self populateGrid];
}

- (void)thumbnailTapped:(UIButton *)sender
{
    NSUInteger idx = sender.tag;
    if (idx < self.mediaMessages.count) {
        NSDictionary *m = self.mediaMessages[idx];
        WAMediaViewController *mv = [[WAMediaViewController alloc] initWithMessage:m];
        mv.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
        [self presentViewController:mv animated:YES completion:nil];
    }
}

@end
