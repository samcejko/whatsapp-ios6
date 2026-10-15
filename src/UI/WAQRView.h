// Renders a QR code (via nayuki qrcodegen) as crisp black modules on white.
#import <UIKit/UIKit.h>

@interface WAQRView : UIView
@property (nonatomic, copy) NSString *text;   // the data to encode; nil clears it
@end
