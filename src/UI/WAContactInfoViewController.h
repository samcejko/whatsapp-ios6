#import <UIKit/UIKit.h>
#import <AddressBookUI/AddressBookUI.h>

@interface WAContactInfoViewController : UIViewController <UITableViewDataSource, UITableViewDelegate, UIActionSheetDelegate, UIAlertViewDelegate, ABPeoplePickerNavigationControllerDelegate>
- (instancetype)initWithJID:(NSString *)jid name:(NSString *)name;
@end
