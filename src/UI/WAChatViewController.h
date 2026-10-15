#import <UIKit/UIKit.h>

@interface WAChatViewController : UIViewController <UITableViewDataSource, UITableViewDelegate, UITextFieldDelegate>
- (instancetype)initWithJID:(NSString *)jid name:(NSString *)name;
@end
