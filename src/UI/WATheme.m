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
    BOOL isBroadcast = [jid rangeOfString:@"@broadcast"].location != NSNotFound;
    [[UIColor whiteColor] set];
    if (isBroadcast) {
        NSString *glyph = @"\U0001F4E2";   // megaphone / broadcast
        UIFont *f = [UIFont systemFontOfSize:d * 0.5];
        CGSize ts = [glyph sizeWithFont:f];
        [glyph drawAtPoint:CGPointMake((d - ts.width) / 2, (d - ts.height) / 2) withFont:f];
    } else if (group || initials.length == 0) {
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

    if ([glyph isEqualToString:@"☎"] || [glyph isEqualToString:@"✓"]) {
        CGFloat iconSz = d * 0.52;
        UIImage *icon = [self callIconWithColor:[UIColor whiteColor] size:CGSizeMake(iconSz, iconSz)];
        [icon drawInRect:CGRectMake((d - iconSz) / 2, (d - iconSz) / 2, iconSz, iconSz)];
    } else if ([glyph isEqualToString:@"▶"]) {
        CGFloat iconSz = d * 0.52;
        UIImage *icon = [self videoCallIconWithColor:[UIColor whiteColor] size:CGSizeMake(iconSz, iconSz)];
        [icon drawInRect:CGRectMake((d - iconSz) / 2, (d - iconSz) / 2, iconSz, iconSz)];
    } else if (glyph.length) {
        UIFont *f = [UIFont boldSystemFontOfSize:d * 0.42];
        CGSize ts = [glyph sizeWithFont:f];
        [[UIColor whiteColor] set];
        [glyph drawAtPoint:CGPointMake((d - ts.width) / 2, (d - ts.height) / 2) withFont:f];
    }

    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
}

+ (UIImage *)roundCallButton:(UIColor *)color diameter:(CGFloat)d type:(WACallButtonType)type
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

    CGFloat iconSz = d * 0.52;
    UIImage *icon = (type == WACallButtonTypeVoice) ?
        [self callIconWithColor:[UIColor whiteColor] size:CGSizeMake(iconSz, iconSz)] :
        [self videoCallIconWithColor:[UIColor whiteColor] size:CGSizeMake(iconSz, iconSz)];
    [icon drawInRect:CGRectMake((d - iconSz) / 2, (d - iconSz) / 2, iconSz, iconSz)];

    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
}

+ (UIImage *)callIconWithColor:(UIColor *)color size:(CGSize)size
{
    UIGraphicsBeginImageContextWithOptions(size, NO, 0);
    CGContextRef c = UIGraphicsGetCurrentContext();
    [color setFill];

    CGFloat w = size.width, h = size.height;
    CGContextSaveGState(c);
    CGContextTranslateCTM(c, w / 2, h / 2);
    CGContextRotateCTM(c, -M_PI_4);

    CGFloat bw = w * 0.75, bh = h * 0.32;
    UIBezierPath *bp = [UIBezierPath bezierPath];
    [bp moveToPoint:CGPointMake(-bw / 2, -bh / 2)];
    [bp addLineToPoint:CGPointMake(-bw / 2 + bw * 0.28, -bh / 2)];
    [bp addQuadCurveToPoint:CGPointMake(bw / 2 - bw * 0.28, -bh / 2) controlPoint:CGPointMake(0, bh * 0.9)];
    [bp addLineToPoint:CGPointMake(bw / 2, -bh / 2)];
    [bp addQuadCurveToPoint:CGPointMake(bw / 2, bh / 2) controlPoint:CGPointMake(bw / 2 + bw * 0.12, 0)];
    [bp addLineToPoint:CGPointMake(bw / 2 - bw * 0.34, bh / 2)];
    [bp addQuadCurveToPoint:CGPointMake(-bw / 2 + bw * 0.34, bh / 2) controlPoint:CGPointMake(0, -bh * 0.15)];
    [bp addLineToPoint:CGPointMake(-bw / 2, bh / 2)];
    [bp addQuadCurveToPoint:CGPointMake(-bw / 2, -bh / 2) controlPoint:CGPointMake(-bw / 2 - bw * 0.12, 0)];
    [bp closePath];
    [bp fill];

    CGContextRestoreGState(c);
    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
}

+ (UIImage *)videoCallIconWithColor:(UIColor *)color size:(CGSize)size
{
    UIGraphicsBeginImageContextWithOptions(size, NO, 0);
    [color setFill];

    CGFloat w = size.width, h = size.height;
    CGFloat bodyW = w * 0.62, bodyH = h * 0.60;
    CGFloat bx = w * 0.05, by = (h - bodyH) / 2;
    UIBezierPath *bp = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(bx, by, bodyW, bodyH) cornerRadius:2.5];
    [bp fill];

    UIBezierPath *lp = [UIBezierPath bezierPath];
    CGFloat lx = bx + bodyW + w * 0.04;
    [lp moveToPoint:CGPointMake(lx, h * 0.36)];
    [lp addLineToPoint:CGPointMake(w * 0.95, h * 0.20)];
    [lp addLineToPoint:CGPointMake(w * 0.95, h * 0.80)];
    [lp addLineToPoint:CGPointMake(lx, h * 0.64)];
    [lp closePath];
    [lp fill];

    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
}

+ (UIImage *)microphoneIconWithColor:(UIColor *)color size:(CGSize)size
{
    UIGraphicsBeginImageContextWithOptions(size, NO, 0);
    CGContextRef c = UIGraphicsGetCurrentContext();
    [color setFill];
    [color setStroke];

    CGFloat w = size.width, h = size.height;
    CGFloat capW = w * 0.44, capH = h * 0.54;
    CGFloat capX = (w - capW) / 2, capY = h * 0.06;
    UIBezierPath *cap = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(capX, capY, capW, capH) cornerRadius:capW / 2];
    [cap fill];

    CGContextSetLineWidth(c, MAX(1.8, w * 0.10));
    CGContextSetLineCap(c, kCGLineCapRound);
    CGFloat cradleR = w * 0.34;
    CGContextAddArc(c, w / 2, capY + capH * 0.65, cradleR, 0, M_PI, 0);
    CGContextStrokePath(c);

    CGFloat stemY1 = capY + capH * 0.65 + cradleR;
    CGFloat stemY2 = h * 0.88;
    CGContextMoveToPoint(c, w / 2, stemY1);
    CGContextAddLineToPoint(c, w / 2, stemY2);
    CGContextStrokePath(c);

    CGContextMoveToPoint(c, w * 0.25, stemY2);
    CGContextAddLineToPoint(c, w * 0.75, stemY2);
    CGContextStrokePath(c);

    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
}

+ (UIImage *)pinIconWithColor:(UIColor *)color size:(CGSize)size
{
    UIGraphicsBeginImageContextWithOptions(size, NO, 0);
    CGContextRef c = UIGraphicsGetCurrentContext();
    [color setFill];
    [color setStroke];

    CGFloat w = size.width, h = size.height;
    CGContextSaveGState(c);
    CGContextTranslateCTM(c, w / 2, h / 2);
    CGContextRotateCTM(c, M_PI_4);

    CGFloat pw = w * 0.30;
    CGContextFillEllipseInRect(c, CGRectMake(-pw * 0.75, -h * 0.42, pw * 1.5, h * 0.22));

    UIBezierPath *cone = [UIBezierPath bezierPath];
    [cone moveToPoint:CGPointMake(-pw * 0.5, -h * 0.20)];
    [cone addLineToPoint:CGPointMake(pw * 0.5, -h * 0.20)];
    [cone addLineToPoint:CGPointMake(pw * 0.85, h * 0.08)];
    [cone addLineToPoint:CGPointMake(-pw * 0.85, h * 0.08)];
    [cone closePath];
    [cone fill];

    CGContextSetLineWidth(c, 1.8);
    CGContextMoveToPoint(c, 0, h * 0.08);
    CGContextAddLineToPoint(c, 0, h * 0.45);
    CGContextStrokePath(c);

    CGContextRestoreGState(c);
    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
}

+ (UIImage *)muteIconWithColor:(UIColor *)color size:(CGSize)size
{
    UIGraphicsBeginImageContextWithOptions(size, NO, 0);
    CGContextRef c = UIGraphicsGetCurrentContext();
    [color setFill];
    [color setStroke];

    CGFloat w = size.width, h = size.height;
    CGFloat bw = w * 0.26, bh = h * 0.34;
    CGFloat bx = w * 0.08, by = (h - bh) / 2;
    UIBezierPath *p = [UIBezierPath bezierPath];
    [p moveToPoint:CGPointMake(bx, by)];
    [p addLineToPoint:CGPointMake(bx + bw, by)];
    [p addLineToPoint:CGPointMake(bx + bw + w * 0.30, h * 0.16)];
    [p addLineToPoint:CGPointMake(bx + bw + w * 0.30, h * 0.84)];
    [p addLineToPoint:CGPointMake(bx + bw, by + bh)];
    [p addLineToPoint:CGPointMake(bx, by + bh)];
    [p closePath];
    [p fill];

    CGContextSetLineWidth(c, 1.8);
    CGContextSetLineCap(c, kCGLineCapRound);
    CGContextMoveToPoint(c, w * 0.10, h * 0.86);
    CGContextAddLineToPoint(c, w * 0.90, h * 0.14);
    CGContextStrokePath(c);

    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
}

+ (UIImage *)clockIconWithColor:(UIColor *)color size:(CGSize)size
{
    UIGraphicsBeginImageContextWithOptions(size, NO, 0);
    CGContextRef c = UIGraphicsGetCurrentContext();
    [color setStroke];
    [color setFill];

    CGFloat w = size.width, h = size.height;
    CGFloat r = MIN(w, h) / 2 - 1.2;
    CGContextSetLineWidth(c, 1.5);
    CGContextStrokeEllipseInRect(c, CGRectMake(w / 2 - r, h / 2 - r, 2 * r, 2 * r));

    CGContextSetLineCap(c, kCGLineCapRound);
    CGContextMoveToPoint(c, w / 2, h / 2);
    CGContextAddLineToPoint(c, w / 2, h / 2 - r * 0.65);
    CGContextMoveToPoint(c, w / 2, h / 2);
    CGContextAddLineToPoint(c, w / 2 + r * 0.50, h / 2);
    CGContextStrokePath(c);

    CGContextFillEllipseInRect(c, CGRectMake(w / 2 - 1, h / 2 - 1, 2, 2));

    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
}

+ (UIImage *)starIconWithColor:(UIColor *)color size:(CGSize)size
{
    UIGraphicsBeginImageContextWithOptions(size, NO, 0);
    [color setFill];

    CGFloat w = size.width, h = size.height;
    CGFloat R = MIN(w, h) / 2 - 1.0;
    CGFloat r = R * 0.42;
    CGPoint center = CGPointMake(w / 2, h / 2);

    UIBezierPath *star = [UIBezierPath bezierPath];
    for (int i = 0; i < 10; i++) {
        CGFloat radius = (i % 2 == 0) ? R : r;
        CGFloat angle = i * M_PI / 5.0 - M_PI_2;
        CGPoint pt = CGPointMake(center.x + radius * cosf(angle), center.y + radius * sinf(angle));
        if (i == 0) [star moveToPoint:pt];
        else [star addLineToPoint:pt];
    }
    [star closePath];
    [star fill];

    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
}

@end
