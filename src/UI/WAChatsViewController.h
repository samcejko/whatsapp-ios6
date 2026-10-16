#import <UIKit/UIKit.h>

@interface WAChatsViewController : UIViewController <UITableViewDataSource, UITableViewDelegate, UIAlertViewDelegate>
@property (nonatomic, strong) UISegmentedControl *navSegment;
- (void)newChat;
- (void)onSegmentChanged;
@end

