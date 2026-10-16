// Shared helpers for the Wazzup app (class prefix WA).
#import <Foundation/Foundation.h>

#define WALoc(key) NSLocalizedString(key, nil)

#ifdef __cplusplus
extern "C" {
#endif
void WALog(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
// The directory the app keeps its session store in (Documents).
NSString *WADocumentsPath(void);
// "HH:mm" for a unix timestamp (message bubble time). Empty for 0.
NSString *WAClockTime(uint64_t ts);
// Chat-list relative time: "HH:mm" today, localized "Yesterday", the weekday this week, else a short date. Empty for 0.
NSString *WAListTime(uint64_t ts);
#ifdef __cplusplus
}
#endif
