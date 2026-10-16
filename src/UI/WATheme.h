// Skeuomorphic iOS 6 look for Wazzup: glossy teal bars, chat wallpaper, speech bubbles.
#import <UIKit/UIKit.h>

typedef NS_ENUM(NSInteger, WAWallpaperType) {
    WAWallpaperTypeClassic = 0,
    WAWallpaperTypeParchment = 1,
    WAWallpaperTypeSlate = 2,
    WAWallpaperTypeSky = 3
};

@interface WATheme : NSObject
+ (UIColor *)header;          // the teal nav/tool bar colour
+ (UIColor *)accent;          // the green send/accent colour
+ (UIColor *)outgoingBubble;  // light green
+ (UIColor *)incomingBubble;  // white
+ (void)styleNavigationBar:(UINavigationBar *)bar;
+ (UIImage *)wallpaper;       // the chat background (tiled light pattern)
+ (WAWallpaperType)currentWallpaperType;
+ (void)setWallpaperType:(WAWallpaperType)type;
+ (UIImage *)wallpaperForType:(WAWallpaperType)type;

// A stretchable glossy speech bubble; outgoing = light green with the tail on the right, else white tail-left.
+ (UIImage *)bubbleOutgoing:(BOOL)outgoing;
+ (UIImage *)glossyButtonWithColor:(UIColor *)color;   // stretchable rounded glossy button background
+ (UIImage *)roundCallButton:(UIColor *)color diameter:(CGFloat)d glyph:(NSString *)glyph;
// A circular avatar: a colour derived from the jid, with the name's initials (or a group glyph for @g.us jids).
+ (UIImage *)avatarForName:(NSString *)name jid:(NSString *)jid diameter:(CGFloat)d;
// A small coloured unread-count pill (green), e.g. "3".
+ (UIImage *)badgeWithCount:(NSInteger)count;
@end
