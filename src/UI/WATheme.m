#import "WATheme.h"
#import <QuartzCore/QuartzCore.h>

@implementation WATheme

+ (UIColor *)header { return [UIColor colorWithRed:0.03 green:0.37 blue:0.33 alpha:1.0]; }        // teal #075E54-ish
+ (UIColor *)accent { return [UIColor colorWithRed:0.09 green:0.56 blue:0.27 alpha:1.0]; }         // green
+ (UIColor *)outgoingBubble { return [UIColor colorWithRed:0.86 green:0.97 blue:0.78 alpha:1.0]; } // #DCF8C6
+ (UIColor *)incomingBubble { return [UIColor whiteColor]; }

+ (void)styleNavigationBar:(UINavigationBar *)bar
{
    if (!bar) return;
    bar.tintColor = [self header];
    bar.barStyle = UIBarStyleBlack;
    if ([bar respondsToSelector:@selector(setTitleTextAttributes:)]) {
        bar.titleTextAttributes = @{ UITextAttributeTextColor: [UIColor whiteColor] };
    }
}

+ (UIImage *)wallpaper
{
    CGSize sz = CGSizeMake(8, 8);
    UIGraphicsBeginImageContextWithOptions(sz, YES, 0);
    CGContextRef c = UIGraphicsGetCurrentContext();
    CGContextSetRGBFillColor(c, 0.91, 0.90, 0.84, 1.0);   // warm beige
    CGContextFillRect(c, CGRectMake(0, 0, sz.width, sz.height));
    CGContextSetRGBFillColor(c, 0.88, 0.87, 0.81, 1.0);
    CGContextFillRect(c, CGRectMake(0, 0, 1, 1));
    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return [img resizableImageWithCapInsets:UIEdgeInsetsZero];
}

+ (UIImage *)bubbleOutgoing:(BOOL)outgoing
{
    CGFloat r = 15, tail = 7, w = 2 * r + tail + 4, h = 2 * r + 4;
    UIGraphicsBeginImageContextWithOptions(CGSizeMake(w, h), NO, 0);
    CGContextRef c = UIGraphicsGetCurrentContext();
    UIColor *fill = outgoing ? [self outgoingBubble] : [self incomingBubble];
    CGRect body = outgoing ? CGRectMake(1, 1, w - tail - 2, h - 2) : CGRectMake(tail + 1, 1, w - tail - 2, h - 2);
    UIBezierPath *p = [UIBezierPath bezierPathWithRoundedRect:body cornerRadius:r];
    // tail
    UIBezierPath *t = [UIBezierPath bezierPath];
    if (outgoing) {
        [t moveToPoint:CGPointMake(w - tail - 2, h - r)];
        [t addLineToPoint:CGPointMake(w - 2, h - 6)];
        [t addLineToPoint:CGPointMake(w - tail - 6, h - 4)];
    } else {
        [t moveToPoint:CGPointMake(tail + 2, h - r)];
        [t addLineToPoint:CGPointMake(2, h - 6)];
        [t addLineToPoint:CGPointMake(tail + 6, h - 4)];
    }
    [t closePath];
    [p appendPath:t];
    CGContextSaveGState(c);
    [p addClip];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    const CGFloat *rgba = CGColorGetComponents(fill.CGColor);
    CGFloat comps[8] = { MIN(rgba[0] + 0.04, 1), MIN(rgba[1] + 0.04, 1), MIN(rgba[2] + 0.04, 1), 1,
                         rgba[0], rgba[1], rgba[2], 1 };
    CGFloat locs[2] = { 0, 1 };
    CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, locs, 2);
    CGContextDrawLinearGradient(c, g, CGPointMake(0, 0), CGPointMake(0, h), 0);
    CGGradientRelease(g);
    CGColorSpaceRelease(cs);
    CGContextRestoreGState(c);
    [[UIColor colorWithWhite:0 alpha:0.10] setStroke];
    p.lineWidth = 0.5;
    [p stroke];
    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    CGFloat capL = outgoing ? r : r + tail, capR = outgoing ? r + tail : r;
    return [img resizableImageWithCapInsets:UIEdgeInsetsMake(r, capL, r, capR)];
}

+ (UIImage *)glossyButtonWithColor:(UIColor *)color
{
    CGFloat w = 24, h = 32, r = 6;
    UIGraphicsBeginImageContextWithOptions(CGSizeMake(w, h), NO, 0);
    CGContextRef c = UIGraphicsGetCurrentContext();
    UIBezierPath *p = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(0.5, 0.5, w - 1, h - 1) cornerRadius:r];
    [p addClip];
    const CGFloat *rgba = CGColorGetComponents(color.CGColor);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGFloat comps[8] = { MIN(rgba[0] + 0.12, 1), MIN(rgba[1] + 0.12, 1), MIN(rgba[2] + 0.12, 1), 1,
                         rgba[0] * 0.82, rgba[1] * 0.82, rgba[2] * 0.82, 1 };
    CGFloat locs[2] = { 0, 1 };
    CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, locs, 2);
    CGContextDrawLinearGradient(c, g, CGPointMake(0, 0), CGPointMake(0, h), 0);
    CGContextSetRGBFillColor(c, 1, 1, 1, 0.22);
    CGContextFillRect(c, CGRectMake(0, 0, w, h / 2));
    CGGradientRelease(g);
    CGColorSpaceRelease(cs);
    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return [img resizableImageWithCapInsets:UIEdgeInsetsMake(r, r, r, r)];
}

+ (UIImage *)roundCallButton:(UIColor *)color diameter:(CGFloat)d glyph:(NSString *)glyph
{
    UIGraphicsBeginImageContextWithOptions(CGSizeMake(d, d), NO, 0);
    CGContextRef c = UIGraphicsGetCurrentContext();
    CGRect rc = CGRectMake(1, 1, d - 2, d - 2);
    CGContextSaveGState(c);
    CGContextAddEllipseInRect(c, rc);
    CGContextClip(c);
    const CGFloat *rgba = CGColorGetComponents(color.CGColor);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGFloat comps[8] = { MIN(rgba[0] + 0.15, 1), MIN(rgba[1] + 0.15, 1), MIN(rgba[2] + 0.15, 1), 1,
                         rgba[0] * 0.75, rgba[1] * 0.75, rgba[2] * 0.75, 1 };
    CGFloat locs[2] = { 0, 1 };
    CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, locs, 2);
    CGContextDrawLinearGradient(c, g, CGPointMake(0, 0), CGPointMake(0, d), 0);
    CGContextSetRGBFillColor(c, 1, 1, 1, 0.25);
    CGContextFillEllipseInRect(c, CGRectMake(d * 0.14, d * 0.08, d * 0.72, d * 0.42));
    CGGradientRelease(g);
    CGColorSpaceRelease(cs);
    CGContextRestoreGState(c);
    if (glyph.length) {
        UIFont *f = [UIFont boldSystemFontOfSize:d * 0.42];
        CGSize ts = [glyph sizeWithFont:f];
        [[UIColor whiteColor] set];
        [glyph drawAtPoint:CGPointMake((d - ts.width) / 2, (d - ts.height) / 2) withFont:f];
    }
    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
}

@end
