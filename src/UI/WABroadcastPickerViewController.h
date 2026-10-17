#import <UIKit/UIKit.h>

@protocol WABroadcastPickerDelegate <NSObject>
- (void)broadcastPickerDidFinishWithRecipients:(NSArray *)recipients;
@end

@interface WABroadcastPickerViewController : UIViewController <UITableViewDataSource, UITableViewDelegate>
@property (nonatomic, weak) id<WABroadcastPickerDelegate> delegate;
@end
