#import "WAMediaViewController.h"
#import "WATheme.h"
#import "../WACommon.h"
#include "wa_history.h"
#include "wa_util.h"

@interface WAMediaViewController ()
@property (nonatomic, strong) NSDictionary *msg;
@property (nonatomic, strong) UIImageView *imageView;
@property (nonatomic, strong) UIActivityIndicatorView *spinner;
@property (nonatomic, strong) UIButton *saveBtn;
@property (nonatomic, strong) UIImage *fullImage;
@end

@implementation WAMediaViewController

- (instancetype)initWithMessage:(NSDictionary *)msg { if ((self = [super init])) { _msg = msg; } return self; }

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];

    self.imageView = [[UIImageView alloc] initWithFrame:self.view.bounds];
    self.imageView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.imageView.contentMode = UIViewContentModeScaleAspectFit;
    NSData *thumb = self.msg[@"thumb"];
    if ([thumb isKindOfClass:[NSData class]]) self.imageView.image = [UIImage imageWithData:thumb];
    [self.view addSubview:self.imageView];

    [self.view addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(dismiss)]];

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

    [self downloadFull];
}

// Download + decrypt the full-resolution image on a background queue, then swap it in.
- (void)downloadFull
{
    NSString *dpath = self.msg[@"dpath"];
    NSData *mkey = self.msg[@"mkey"];
    NSData *esha = self.msg[@"esha"];
    int kind = [self.msg[@"kind"] intValue];
    if (kind != WA_MSG_IMAGE || ![dpath isKindOfClass:[NSString class]] || ![mkey isKindOfClass:[NSData class]] || mkey.length != 32)
        return;   // no refs (or not an image): the thumbnail is all we can show

    [self.spinner startAnimating];
    const char *cdpath = strdup(dpath.UTF8String);
    NSData *mk = mkey, *es = [esha isKindOfClass:[NSData class]] ? esha : nil;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        wa_buf out;
        wa_buf_init(&out);
        int rc = wa_media_download(cdpath, mk.bytes, es.length ? es.bytes : NULL, es.length, kind, NULL, &out);
        UIImage *img = (rc == 0 && out.len) ? [UIImage imageWithData:[NSData dataWithBytes:out.data length:out.len]] : nil;
        wa_buf_free(&out);
        free((void *)cdpath);
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.spinner stopAnimating];
            if (img) { self.fullImage = img; self.imageView.image = img; self.saveBtn.hidden = NO; }
        });
    });
}

- (void)save
{
    if (self.fullImage) UIImageWriteToSavedPhotosAlbum(self.fullImage, nil, nil, NULL);
    self.saveBtn.enabled = NO;
}

- (void)dismiss { [self dismissViewControllerAnimated:YES completion:nil]; }

- (BOOL)prefersStatusBarHidden { return YES; }

@end
