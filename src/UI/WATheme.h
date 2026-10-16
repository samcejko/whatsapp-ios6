// Skeuomorphic iOS 6 look for Wazzup: glossy teal bars, chat wallpaper, speech bubbles.
#import <UIKit/UIKit.h>

@interface WATheme : NSObject
+ (UIColor *)header;          // the teal nav/tool bar colour
+ (UIColor *)accent;          // the green send/accent colour
+ (UIColor *)outgoingBubble;  // light green
+ (UIColor *)incomingBubble;  // white
+ (void)styleNavigationBar:(UINavigationBar *)bar;
+ (UIImage *)wallpaper;       // the chat background (tiled light pattern)
// A stretchable glossy speech bubble; outgoing = light green with the tail on the right, else white tail-left.
+ (UIImage *)bubbleOutgoing:(BOOL)outgoing;
+ (UIImage *)glossyButtonWithColor:(UIColor *)color;   // stretchable rounded glossy button background
+ (UIImage *)roundCallButton:(UIColor *)color diameter:(CGFloat)d glyph:(NSString *)glyph;
@end
