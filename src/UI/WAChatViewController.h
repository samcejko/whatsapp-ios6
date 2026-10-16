#import <UIKit/UIKit.h>

@interface WAChatViewController : UIViewController <UITableViewDataSource, UITableViewDelegate, UITextFieldDelegate, UIImagePickerControllerDelegate, UINavigationControllerDelegate, UIActionSheetDelegate>
- (instancetype)initWithJID:(NSString *)jid name:(NSString *)name;
- (void)toggleSearch;
- (void)searchBar:(UISearchBar *)searchBar textDidChange:(NSString *)searchText;
@end

