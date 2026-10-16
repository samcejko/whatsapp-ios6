#import "WATheme.h"
#import "../WACommon.h"
#import "../WAChatStore.h"
#import <QuartzCore/QuartzCore.h>

@implementation WATheme

+ (UIColor *)header { return [UIColor colorWithRed:0.03 green:0.37 blue:0.33 alpha:1.0]; }        // teal #075E54-ish
+ (UIColor *)accent { return [UIColor colorWithRed:0.09 green:0.56 blue:0.27 alpha:1.0]; }         // green
+ (UIColor *)outgoingBubble { return [UIColor colorWithRed:0.85 green:0.95 blue:0.74 alpha:1.0]; } // #D9F2BD light green
+ (UIColor *)incomingBubble { return [UIColor colorWithRed:1.0 green:1.0 blue:1.0 alpha:1.0]; }    // white (RGB, never grayscale)

+ (void)styleNavigationBar:(UINavigationBar *)bar
{
    if (!bar) return;
    bar.tintColor = [self header];
    bar.barStyle = UIBarStyleBlack;
    if ([bar respondsToSelector:@selector(setTitleTextAttributes:)]) {
        bar.titleTextAttributes = @{ UITextAttributeTextColor: [UIColor whiteColor] };
    }
}

+ (WAWallpaperType)currentWallpaperType
{
    NSNumber *n = [[NSUserDefaults standardUserDefaults] objectForKey:@"wa_wallpaper_type"];
    return n ? [n integerValue] : WAWallpaperTypeClassic;
}

+ (void)setWallpaperType:(WAWallpaperType)type
{
    [[NSUserDefaults standardUserDefaults] setInteger:type forKey:@"wa_wallpaper_type"];
    [[NSUserDefaults standardUserDefaults] synchronize];
    [[NSNotificationCenter defaultCenter] postNotificationName:@"WAWallpaperChangedNotification" object:nil];
}

+ (UIImage *)wallpaperForType:(WAWallpaperType)type
{
    CGSize sz = CGSizeMake(16, 16);
    UIGraphicsBeginImageContextWithOptions(sz, YES, 0);
    CGContextRef c = UIGraphicsGetCurrentContext();
    if (type == WAWallpaperTypeParchment) {
        CGContextSetRGBFillColor(c, 0.95, 0.92, 0.82, 1.0);
        CGContextFillRect(c, CGRectMake(0, 0, sz.width, sz.height));
        CGContextSetRGBFillColor(c, 0.92, 0.88, 0.77, 1.0);
        CGContextFillRect(c, CGRectMake(0, 0, 2, 2));
        CGContextFillRect(c, CGRectMake(8, 8, 2, 2));
    } else if (type == WAWallpaperTypeSlate) {
        CGContextSetRGBFillColor(c, 0.18, 0.22, 0.25, 1.0);
        CGContextFillRect(c, CGRectMake(0, 0, sz.width, sz.height));
        CGContextSetRGBFillColor(c, 0.22, 0.26, 0.30, 1.0);
        CGContextFillRect(c, CGRectMake(0, 0, 2, 2));
        CGContextFillRect(c, CGRectMake(8, 8, 2, 2));
    } else if (type == WAWallpaperTypeSky) {
        CGContextSetRGBFillColor(c, 0.85, 0.92, 0.98, 1.0);
        CGContextFillRect(c, CGRectMake(0, 0, sz.width, sz.height));
        CGContextSetRGBFillColor(c, 0.78, 0.87, 0.95, 1.0);
        CGContextFillRect(c, CGRectMake(0, 0, 2, 2));
        CGContextFillRect(c, CGRectMake(8, 8, 2, 2));
    } else { // Classic
        CGContextSetRGBFillColor(c, 0.91, 0.90, 0.84, 1.0);
        CGContextFillRect(c, CGRectMake(0, 0, sz.width, sz.height));
        CGContextSetRGBFillColor(c, 0.88, 0.87, 0.81, 1.0);
        CGContextFillRect(c, CGRectMake(0, 0, 2, 2));
        CGContextFillRect(c, CGRectMake(8, 8, 2, 2));
    }
    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return [img resizableImageWithCapInsets:UIEdgeInsetsZero];
}

+ (UIImage *)wallpaper
{
    return [self wallpaperForType:[self currentWallpaperType]];
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
    // Robustly read RGBA: whiteColor et al. are in the grayscale space (2 components), so CGColorGetComponents
    // would read out of bounds. getRed:…/getWhite:… normalise any UIColor to RGB.
    CGFloat rr, gg, bb, aa;
    if (![fill getRed:&rr green:&gg blue:&bb alpha:&aa]) { CGFloat ww = 1; [fill getWhite:&ww alpha:&aa]; rr = gg = bb = ww; }
    CGFloat comps[8] = { MIN(rr + 0.04, 1), MIN(gg + 0.04, 1), MIN(bb + 0.04, 1), 1,
                         rr, gg, bb, 1 };
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

+ (UIImage *)avatarForName:(NSString *)name jid:(NSString *)jid diameter:(CGFloat)d
{
    if (jid.length) {
        NSString *key = [WAChatStore keyForJID:jid];
        NSString *p1 = [WADocumentsPath() stringByAppendingPathComponent:[NSString stringWithFormat:@"wa6_avatar_%@.jpg", key]];
        UIImage *cached = [UIImage imageWithContentsOfFile:p1];
        if (!cached) {
            NSString *p2 = [WADocumentsPath() stringByAppendingPathComponent:[NSString stringWithFormat:@"wa6_avatar_%@.png", key]];
            cached = [UIImage imageWithContentsOfFile:p2];
        }
        if (cached && cached.size.width > 0 && cached.size.height > 0) {
            UIGraphicsBeginImageContextWithOptions(CGSizeMake(d, d), NO, 0);
            CGContextRef ctx = UIGraphicsGetCurrentContext();
            CGRect rc = CGRectMake(0, 0, d, d);
            CGContextSaveGState(ctx);
            CGContextAddEllipseInRect(ctx, rc);
            CGContextClip(ctx);
            CGFloat scale = MAX(d / cached.size.width, d / cached.size.height);
            CGFloat w = cached.size.width * scale;
            CGFloat h = cached.size.height * scale;
            [cached drawInRect:CGRectMake((d - w) / 2, (d - h) / 2, w, h)];
            CGContextRestoreGState(ctx);
            UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
            UIGraphicsEndImageContext();
            if (img) return img;
        }
    }

    // A stable colour per contact, from a hash of the jid.
    static const CGFloat palette[8][3] = {
        { 0.38, 0.49, 0.85 }, { 0.17, 0.63, 0.60 }, { 0.85, 0.44, 0.34 }, { 0.55, 0.45, 0.76 },
        { 0.90, 0.60, 0.20 }, { 0.30, 0.66, 0.38 }, { 0.80, 0.38, 0.56 }, { 0.36, 0.58, 0.68 },
    };
    unsigned h = 5381;
    for (NSUInteger i = 0; i < jid.length; i++) h = ((h << 5) + h) + [jid characterAtIndex:i];
    const CGFloat *rgb = palette[h & 7];

    BOOL group = [jid rangeOfString:@"@g.us"].location != NSNotFound;
    NSString *initials = @"";
    if (!group) {
        // Only real (letter) initials; a number-named chat gets the person glyph instead of a lone digit.
        NSCharacterSet *letters = [NSCharacterSet letterCharacterSet];
        NSArray *parts = [[name stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] componentsSeparatedByString:@" "];
        NSMutableString *ini = [NSMutableString string];
        for (NSString *p in parts) {
            if (p.length && ini.length < 2 && [letters characterIsMember:[p characterAtIndex:0]])
                [ini appendString:[[p substringToIndex:1] uppercaseString]];
        }
        initials = ini;
    }

    UIGraphicsBeginImageContextWithOptions(CGSizeMake(d, d), NO, 0);
    CGContextRef c = UIGraphicsGetCurrentContext();
    CGRect rc = CGRectMake(0, 0, d, d);
    CGContextSaveGState(c);
    CGContextAddEllipseInRect(c, rc);
    CGContextClip(c);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGFloat comps[8] = { MIN(rgb[0] + 0.12, 1), MIN(rgb[1] + 0.12, 1), MIN(rgb[2] + 0.12, 1), 1, rgb[0] * 0.9, rgb[1] * 0.9, rgb[2] * 0.9, 1 };
    CGFloat locs[2] = { 0, 1 };
    CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, locs, 2);
    CGContextDrawLinearGradient(c, g, CGPointMake(0, 0), CGPointMake(0, d), 0);
    CGGradientRelease(g);
    CGColorSpaceRelease(cs);
    CGContextRestoreGState(c);
    [[UIColor whiteColor] set];
    if (group || initials.length == 0) {
        NSString *glyph = group ? @"\U0001F465" : @"\U0001F464";   // group / person
        UIFont *f = [UIFont systemFontOfSize:d * 0.5];
        CGSize ts = [glyph sizeWithFont:f];
        [glyph drawAtPoint:CGPointMake((d - ts.width) / 2, (d - ts.height) / 2) withFont:f];
    } else {
        UIFont *f = [UIFont boldSystemFontOfSize:d * 0.4];
        CGSize ts = [initials sizeWithFont:f];
        [initials drawAtPoint:CGPointMake((d - ts.width) / 2, (d - ts.height) / 2) withFont:f];
    }
    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
}

+ (UIImage *)badgeWithCount:(NSInteger)count
{
    NSString *s = count > 99 ? @"99+" : [NSString stringWithFormat:@"%ld", (long)count];
    UIFont *f = [UIFont boldSystemFontOfSize:13];
    CGSize ts = [s sizeWithFont:f];
    CGFloat h = 20, w = MAX(h, ts.width + 12);
    UIGraphicsBeginImageContextWithOptions(CGSizeMake(w, h), NO, 0);
    CGContextRef c = UIGraphicsGetCurrentContext();
    UIBezierPath *p = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, w, h) cornerRadius:h / 2];
    [p addClip];
    UIColor *a = [self accent];
    CGFloat rr, gg, bb, aa;
    [a getRed:&rr green:&gg blue:&bb alpha:&aa];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGFloat comps[8] = { MIN(rr + 0.1, 1), MIN(gg + 0.1, 1), MIN(bb + 0.1, 1), 1, rr * 0.85, gg * 0.85, bb * 0.85, 1 };
    CGFloat locs[2] = { 0, 1 };
    CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, locs, 2);
    CGContextDrawLinearGradient(c, g, CGPointMake(0, 0), CGPointMake(0, h), 0);
    CGGradientRelease(g);
    CGColorSpaceRelease(cs);
    [[UIColor whiteColor] set];
    [s drawAtPoint:CGPointMake((w - ts.width) / 2, (h - ts.height) / 2) withFont:f];
    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
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
