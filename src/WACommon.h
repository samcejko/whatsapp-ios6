// Shared helpers for the Wazzup app (class prefix WA).
#import <Foundation/Foundation.h>

#define WALoc(key) NSLocalizedString(key, nil)

#ifdef __cplusplus
extern "C" {
#endif
void WALog(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
// The directory the app keeps its session store in (Documents).
NSString *WADocumentsPath(void);
#ifdef __cplusplus
}
#endif
