#import <UIKit/UIKit.h>

@interface WAContactInfoViewController : UIViewController <UITableViewDataSource, UITableViewDelegate, UIActionSheetDelegate>
- (instancetype)initWithJID:(NSString *)jid name:(NSString *)name;
@end
