#import "WAQRView.h"
#include "qrcodegen.h"

@implementation WAQRView

- (instancetype)initWithFrame:(CGRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        self.backgroundColor = [UIColor whiteColor];
        self.contentMode = UIViewContentModeRedraw;
    }
    return self;
}

- (void)setText:(NSString *)text
{
    _text = [text copy];
    [self setNeedsDisplay];
}

- (void)drawRect:(CGRect)rect
{
    CGContextRef c = UIGraphicsGetCurrentContext();
    CGContextSetRGBFillColor(c, 1, 1, 1, 1);
    CGContextFillRect(c, rect);
    const char *t = self.text.length ? [self.text UTF8String] : NULL;
    if (!t) return;

    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(t, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        return;
    }
    int n = qrcodegen_getSize(qr);
    CGFloat side = MIN(rect.size.width, rect.size.height);
    CGFloat scale = floor((side - 8) / n);
    if (scale < 1) scale = 1;
    CGFloat total = scale * n;
    CGFloat ox = (rect.size.width - total) / 2, oy = (rect.size.height - total) / 2;
    CGContextSetRGBFillColor(c, 0, 0, 0, 1);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            if (qrcodegen_getModule(qr, x, y)) {
                CGContextFillRect(c, CGRectMake(ox + x * scale, oy + y * scale, scale, scale));
            }
        }
    }
}

@end
