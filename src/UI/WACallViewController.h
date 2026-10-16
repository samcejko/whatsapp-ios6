#import <UIKit/UIKit.h>

// The iOS 6 Phone/FaceTime-style call screen: avatar, name, status and the round call controls.
@interface WACallViewController : UIViewController
- (instancetype)initWithName:(NSString *)name video:(BOOL)video incoming:(BOOL)incoming;
- (instancetype)initWithJID:(NSString *)jid name:(NSString *)name video:(BOOL)video incoming:(BOOL)incoming;
@end
