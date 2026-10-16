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
// Day label for a date separator: localized "Today"/"Yesterday", the weekday this week, else a medium date.
NSString *WADayLabel(uint64_t ts);
// Resolve contact name from device address book by phone number / JID user part
NSString *WAAddressBookNameForNumber(NSString *number);
// Checks if a name string is nil, empty, or consists only of digits / phone symbols
BOOL WAIsRawNumberOrEmpty(NSString *name);
// Pretty-formats a raw phone number / JID (e.g. +420 773 047 722) or returns "WhatsApp kontakt" for LID
NSString *WAFormatPhoneNumber(NSString *raw);
// Unified display name resolver: saved name -> AddressBook -> WhatsApp contact / formatted phone number
NSString *WADisplayNameForChat(NSString *jid, NSString *savedName);
// Invalidate address book in-memory cache so it re-reads
void WAReloadAddressBookCache(void);
// Parse WhatsApp markdown (*bold*, _italic_, ~strike~, `code`) into an NSAttributedString
NSAttributedString *WAParseMarkdownText(NSString *raw, UIFont *baseFont, UIColor *textColor);
// Measure the bounding size of an NSAttributedString constrained to maxWidth using CoreText
CGSize WAMeasureAttributedString(NSAttributedString *as, CGFloat maxWidth);
#ifdef __cplusplus
}
#endif

