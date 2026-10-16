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

typedef NS_ENUM(NSInteger, WACallButtonType) {
    WACallButtonTypeVoice = 0,
    WACallButtonTypeVideo = 1
};

// A stretchable glossy speech bubble; outgoing = light green with the tail on the right, else white tail-left.
+ (UIImage *)bubbleOutgoing:(BOOL)outgoing;
+ (UIImage *)glossyButtonWithColor:(UIColor *)color;   // stretchable rounded glossy button background
+ (UIImage *)roundCallButton:(UIColor *)color diameter:(CGFloat)d glyph:(NSString *)glyph;
+ (UIImage *)roundCallButton:(UIColor *)color diameter:(CGFloat)d type:(WACallButtonType)type;

// Native iOS 6 style vector icons (replaces emojis)
+ (UIImage *)callIconWithColor:(UIColor *)color size:(CGSize)size;
+ (UIImage *)videoCallIconWithColor:(UIColor *)color size:(CGSize)size;
+ (UIImage *)microphoneIconWithColor:(UIColor *)color size:(CGSize)size;
+ (UIImage *)pinIconWithColor:(UIColor *)color size:(CGSize)size;
+ (UIImage *)muteIconWithColor:(UIColor *)color size:(CGSize)size;
+ (UIImage *)clockIconWithColor:(UIColor *)color size:(CGSize)size;
+ (UIImage *)starIconWithColor:(UIColor *)color size:(CGSize)size;

// A circular avatar: a colour derived from the jid, with the name's initials (or a group glyph for @g.us jids).
+ (UIImage *)avatarForName:(NSString *)name jid:(NSString *)jid diameter:(CGFloat)d;
// A small coloured unread-count pill (green), e.g. "3".
+ (UIImage *)badgeWithCount:(NSInteger)count;
@end
