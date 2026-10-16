#import "WAMediaViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#import <MediaPlayer/MediaPlayer.h>
#include "wa_history.h"
#include "wa_util.h"

@interface WAMediaViewController ()
@property (nonatomic, strong) NSDictionary *msg;
@property (nonatomic, strong) UIImageView *imageView;
@property (nonatomic, strong) UIActivityIndicatorView *spinner;
@property (nonatomic, strong) UIButton *saveBtn;
@property (nonatomic, strong) UIButton *playBtn;
@property (nonatomic, strong) UIImage *fullImage;
@property (nonatomic, copy) NSString *videoPath;
@property (nonatomic, strong) MPMoviePlayerViewController *moviePlayer;
@end

@implementation WAMediaViewController

- (instancetype)initWithMessage:(NSDictionary *)msg { if ((self = [super init])) { _msg = msg; } return self; }

- (instancetype)initWithImage:(UIImage *)image
{
    if ((self = [super init])) {
        _fullImage = image;
    }
    return self;
}

- (NSString *)videoCachePath
{
    NSString *mid = self.msg[@"id"];
    if (!mid.length) mid = [NSString stringWithFormat:@"%llu", [self.msg[@"ts"] unsignedLongLongValue]];
    return [WADocumentsPath() stringByAppendingPathComponent:[NSString stringWithFormat:@"wa6_vid_%@.mp4", mid]];
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];

    self.imageView = [[UIImageView alloc] initWithFrame:self.view.bounds];
    self.imageView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.imageView.contentMode = UIViewContentModeScaleAspectFit;
    if (self.fullImage) {
        self.imageView.image = self.fullImage;
    } else {
        NSData *thumb = self.msg[@"thumb"];
        if ([thumb isKindOfClass:[NSData class]]) self.imageView.image = [UIImage imageWithData:thumb];
    }
    [self.view addSubview:self.imageView];

    [self.view addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(onViewTapped)]];

    UIButton *close = [UIButton buttonWithType:UIButtonTypeCustom];
    [close setTitle:@"✕" forState:UIControlStateNormal];
    close.titleLabel.font = [UIFont systemFontOfSize:26];
    close.frame = CGRectMake(6, 18, 44, 44);
    [close addTarget:self action:@selector(dismiss) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:close];

    self.saveBtn = [UIButton buttonWithType:UIButtonTypeCustom];
    [self.saveBtn setTitle:WALoc(@"save_to_photos") forState:UIControlStateNormal];
    [self.saveBtn setBackgroundImage:[WATheme glossyButtonWithColor:[WATheme accent]] forState:UIControlStateNormal];
    self.saveBtn.titleLabel.font = [UIFont boldSystemFontOfSize:15];
    self.saveBtn.frame = CGRectMake((self.view.bounds.size.width - 200) / 2, self.view.bounds.size.height - 60, 200, 40);
    self.saveBtn.autoresizingMask = UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
    self.saveBtn.hidden = YES;
    [self.saveBtn addTarget:self action:@selector(save) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:self.saveBtn];

    self.spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleWhiteLarge];
    self.spinner.center = CGPointMake(self.view.bounds.size.width / 2, self.view.bounds.size.height / 2);
    self.spinner.autoresizingMask = UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin | UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
    [self.view addSubview:self.spinner];

    if (self.fullImage) {
        self.saveBtn.hidden = NO;
        return;
    }

    int kind = [self.msg[@"kind"] intValue];
    if (kind == WA_MSG_VIDEO) {
        NSString *vp = [self videoCachePath];
        if ([[NSFileManager defaultManager] fileExistsAtPath:vp]) {
            self.videoPath = vp;
            self.saveBtn.hidden = NO;
        }
        self.playBtn = [UIButton buttonWithType:UIButtonTypeCustom];
        [self.playBtn setTitle:@"▶" forState:UIControlStateNormal];
        self.playBtn.titleLabel.font = [UIFont systemFontOfSize:54];
        self.playBtn.frame = CGRectMake((self.view.bounds.size.width - 80) / 2, (self.view.bounds.size.height - 80) / 2, 80, 80);
        self.playBtn.autoresizingMask = UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin | UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
        [self.playBtn addTarget:self action:@selector(playVideo) forControlEvents:UIControlEventTouchUpInside];
        [self.view addSubview:self.playBtn];
    } else {
        [self downloadFull];
    }
}

// Download + decrypt the full-resolution image or video on a background queue.
- (void)downloadFull
{
    NSString *dpath = self.msg[@"dpath"];
    NSData *mkey = self.msg[@"mkey"];
    NSData *esha = self.msg[@"esha"];
    int kind = [self.msg[@"kind"] intValue];
    if ((kind != WA_MSG_IMAGE && kind != WA_MSG_VIDEO) ||
        ![dpath isKindOfClass:[NSString class]] ||
        ![mkey isKindOfClass:[NSData class]] || mkey.length != 32)
        return;

    [self.spinner startAnimating];
    const char *cdpath = strdup(dpath.UTF8String);
    NSData *mk = mkey, *es = [esha isKindOfClass:[NSData class]] ? esha : nil;
    NSString *vp = (kind == WA_MSG_VIDEO) ? [self videoCachePath] : nil;

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        wa_buf out;
        wa_buf_init(&out);
        int rc = wa_media_download(cdpath, mk.bytes, es.length ? es.bytes : NULL, es.length, kind, NULL, &out);
        free((void *)cdpath);
        if (rc == 0 && out.len > 0) {
            if (kind == WA_MSG_IMAGE) {
                UIImage *img = [UIImage imageWithData:[NSData dataWithBytes:out.data length:out.len]];
                wa_buf_free(&out);
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self.spinner stopAnimating];
                    if (img) { self.fullImage = img; self.imageView.image = img; self.saveBtn.hidden = NO; }
                });
            } else if (kind == WA_MSG_VIDEO) {
                NSData *vd = [NSData dataWithBytes:out.data length:out.len];
                wa_buf_free(&out);
                [vd writeToFile:vp atomically:YES];
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self.spinner stopAnimating];
                    self.videoPath = vp;
                    self.saveBtn.hidden = NO;
                    [self playVideo];
                });
            }
        } else {
            wa_buf_free(&out);
            dispatch_async(dispatch_get_main_queue(), ^{
                [self.spinner stopAnimating];
            });
        }
    });
}

- (void)playVideo
{
    if (self.videoPath.length && [[NSFileManager defaultManager] fileExistsAtPath:self.videoPath]) {
        self.moviePlayer = [[MPMoviePlayerViewController alloc] initWithContentURL:[NSURL fileURLWithPath:self.videoPath]];
        [self presentMoviePlayerViewControllerAnimated:self.moviePlayer];
        return;
    }
    if (!self.spinner.isAnimating) {
        [self downloadFull];
    }
}

- (void)onViewTapped
{
    int kind = [self.msg[@"kind"] intValue];
    if (kind == WA_MSG_VIDEO) {
        [self playVideo];
    } else {
        [self dismiss];
    }
}

- (void)save
{
    if (self.fullImage) {
        UIImageWriteToSavedPhotosAlbum(self.fullImage, nil, nil, NULL);
        self.saveBtn.enabled = NO;
    } else if (self.videoPath.length && UIVideoAtPathIsCompatibleWithSavedPhotosAlbum(self.videoPath)) {
        UISaveVideoAtPathToSavedPhotosAlbum(self.videoPath, nil, nil, NULL);
        self.saveBtn.enabled = NO;
    }
}

- (void)dismiss { [self dismissViewControllerAnimated:YES completion:nil]; }

- (BOOL)prefersStatusBarHidden { return YES; }

@end
