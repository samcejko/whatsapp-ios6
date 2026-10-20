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
+ (UIImage *)contactIconWithColor:(UIColor *)color size:(CGSize)size;

// A circular avatar: a colour derived from the jid, with the name's initials (or a group glyph for @g.us jids).
+ (UIImage *)avatarForName:(NSString *)name jid:(NSString *)jid diameter:(CGFloat)d;
// A small coloured unread-count pill (green), e.g. "3".
+ (UIImage *)badgeWithCount:(NSInteger)count;
@end

// Semantic Colors - WhatsApp-inspired but legally distinct
@interface WATheme (SemanticColors)

// Primary Actions
+ (UIColor *)primaryActionColor;      // Main action color (green)
+ (UIColor *)secondaryActionColor;    // Secondary action color

// Status Colors
+ (UIColor *)successColor;            // Green for success/checkmarks
+ (UIColor *)errorColor;              // Red for errors
+ (UIColor *)warningColor;            // Yellow/Orange for warnings
+ (UIColor *)infoColor;               // Blue for info

// Message Status Colors
+ (UIColor *)messagePendingColor;     // Gray for pending
+ (UIColor *)messageSentColor;        // Light gray for sent
+ (UIColor *)messageDeliveredColor;   // Blue for delivered
+ (UIColor *)messageReadColor;        // Blue for read (double check)

// Chat Colors
+ (UIColor *)chatBackgroundColor;     // Chat background
+ (UIColor *)outgoingBubbleColor;     // Outgoing message bubble
+ (UIColor *)incomingBubbleColor;     // Incoming message bubble
+ (UIColor *)quotedMessageBarColor;   // Quoted message accent bar

@end

// Layout Constants - 8pt grid system
@interface WATheme (Layout)

// Spacing
+ (CGFloat)standardSpacing;           // 8pt
+ (CGFloat)compactSpacing;            // 4pt
+ (CGFloat)largeSpacing;              // 16pt

// Bubble Styling
+ (CGFloat)bubbleCornerRadius;        // 8pt
+ (CGFloat)bubbleMaxWidthRatio;       // 0.75 (75% of screen width)
+ (UIEdgeInsets)bubblePadding;        // 8pt all sides

// Avatar Sizes
+ (CGFloat)avatarSizeLarge;           // 64pt (profile)
+ (CGFloat)avatarSizeMedium;          // 44pt (chat list)
+ (CGFloat)avatarSizeSmall;           // 32pt (group participants)

@end

// Typography System
@interface WATheme (Typography)

// Text Styles
+ (UIFont *)headlineFont;             // Bold 17pt
+ (UIFont *)bodyFont;                 // Regular 16pt
+ (UIFont *)captionFont;              // Regular 13pt
+ (UIFont *)timestampFont;            // Regular 11pt

// Message Fonts
+ (UIFont *)messageBubbleFont;        // Regular 16pt
+ (UIFont *)messageTimestampFont;     // Regular 11pt
+ (UIFont *)messageNameFont;          // Bold 14pt (groups)

@end

// Animation Helpers
@interface WATheme (Animations)

+ (NSTimeInterval)standardAnimationDuration;
+ (UIViewAnimationOptions)standardAnimationOptions;
+ (void)animateWithStandardTiming:(void (^)(void))animations completion:(void (^)(BOOL))completion;

@end
