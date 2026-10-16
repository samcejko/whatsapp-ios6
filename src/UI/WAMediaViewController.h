// Full-screen photo viewer. Shows the inline thumbnail immediately, then downloads + decrypts the full-resolution
// image from the CDN (when the message carries download refs) and lets the user save it to Photos.
#import <UIKit/UIKit.h>

@interface WAMediaViewController : UIViewController
// msg: a chat-store message dict with @"thumb" (NSData), and optionally @"dpath"/@"mkey"/@"esha"/@"kind" for full-res.
- (instancetype)initWithMessage:(NSDictionary *)msg;
- (instancetype)initWithImage:(UIImage *)image;
@end
