#import <UIKit/UIKit.h>

@interface WAContactInfoViewController : UIViewController <UITableViewDataSource, UITableViewDelegate, UIActionSheetDelegate, UIAlertViewDelegate>
- (instancetype)initWithJID:(NSString *)jid name:(NSString *)name;
@end
