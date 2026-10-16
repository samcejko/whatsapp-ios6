#import "WACommon.h"
#import <AddressBook/AddressBook.h>
#include "wa_util.h"

void WALog(NSString *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSLog(@"[Wazzup] %@", s);
}

NSString *WADocumentsPath(void)
{
    NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return dirs.count ? dirs[0] : NSTemporaryDirectory();
}

NSString *WAClockTime(uint64_t ts)
{
    if (!ts) return @"";
    static NSDateFormatter *f;
    if (!f) { f = [[NSDateFormatter alloc] init]; f.dateFormat = @"HH:mm"; }
    return [f stringFromDate:[NSDate dateWithTimeIntervalSince1970:ts]];
}

NSString *WAListTime(uint64_t ts)
{
    if (!ts) return @"";
    NSDate *date = [NSDate dateWithTimeIntervalSince1970:ts];
    NSCalendar *cal = [NSCalendar currentCalendar];
    NSInteger daysUnit = NSDayCalendarUnit | NSMonthCalendarUnit | NSYearCalendarUnit;
    NSDateComponents *dc = [cal components:daysUnit fromDate:date];
    NSDateComponents *dn = [cal components:daysUnit fromDate:[NSDate date]];
    static NSDateFormatter *clock, *wday, *dshort;
    if (!clock) {
        clock = [[NSDateFormatter alloc] init]; clock.dateFormat = @"HH:mm";
        wday = [[NSDateFormatter alloc] init]; wday.dateFormat = @"EEEE";
        dshort = [[NSDateFormatter alloc] init]; dshort.dateStyle = NSDateFormatterShortStyle; dshort.timeStyle = NSDateFormatterNoStyle;
    }
    if (dc.year == dn.year && dc.month == dn.month && dc.day == dn.day) return [clock stringFromDate:date];
    NSTimeInterval diff = [[NSDate date] timeIntervalSinceDate:date];
    if (diff < 2 * 86400 && dc.day != dn.day) return WALoc(@"yesterday");
    if (diff < 7 * 86400) return [wday stringFromDate:date];
    return [dshort stringFromDate:date];
}

NSString *WADayLabel(uint64_t ts)
{
    if (!ts) return @"";
    NSDate *date = [NSDate dateWithTimeIntervalSince1970:ts];
    NSCalendar *cal = [NSCalendar currentCalendar];
    NSInteger u = NSDayCalendarUnit | NSMonthCalendarUnit | NSYearCalendarUnit;
    NSDateComponents *dc = [cal components:u fromDate:date];
    NSDateComponents *dn = [cal components:u fromDate:[NSDate date]];
    static NSDateFormatter *wday, *dmed;
    if (!wday) {
        wday = [[NSDateFormatter alloc] init]; wday.dateFormat = @"EEEE";
        dmed = [[NSDateFormatter alloc] init]; dmed.dateStyle = NSDateFormatterMediumStyle; dmed.timeStyle = NSDateFormatterNoStyle;
    }
    if (dc.year == dn.year && dc.month == dn.month && dc.day == dn.day) return WALoc(@"today");
    NSTimeInterval diff = [[NSDate date] timeIntervalSinceDate:date];
    if (diff < 2 * 86400 && dc.day != dn.day) return WALoc(@"yesterday");
    if (diff < 7 * 86400) return [wday stringFromDate:date];
    return [dmed stringFromDate:date];
}

static NSDictionary *s_abCache = nil;

void WAReloadAddressBookCache(void)
{
    @synchronized([WACommon class]) {
        s_abCache = nil;
    }
}

static void ensureAddressBookCache(void)
{
    @synchronized([WACommon class]) {
        if (s_abCache) return;
        NSMutableDictionary *dict = [NSMutableDictionary dictionary];
        CFErrorRef err = NULL;
        ABAddressBookRef ab = NULL;
        if (&ABAddressBookCreateWithOptions != NULL) {
            ab = ABAddressBookCreateWithOptions(NULL, &err);
        }
        if (!ab) {
            s_abCache = [dict copy];
            return;
        }
        CFArrayRef people = ABAddressBookCopyArrayOfAllPeople(ab);
        if (people) {
            CFIndex count = CFArrayGetCount(people);
            for (CFIndex i = 0; i < count; i++) {
                ABRecordRef person = CFArrayGetValueAtIndex(people, i);
                CFStringRef fn = ABRecordCopyCompositeName(person);
                NSString *name = fn ? [NSString stringWithString:(__bridge NSString *)fn] : nil;
                if (fn) CFRelease(fn);
                if (!name.length) continue;

                ABMultiValueRef phones = ABRecordCopyValue(person, kABPersonPhoneProperty);
                if (phones) {
                    CFIndex phoneCount = ABMultiValueGetCount(phones);
                    for (CFIndex p = 0; p < phoneCount; p++) {
                        CFStringRef phVal = ABMultiValueCopyValueAtIndex(phones, p);
                        if (phVal) {
                            NSString *phStr = (__bridge NSString *)phVal;
                            NSMutableString *digits = [NSMutableString string];
                            for (NSUInteger k = 0; k < phStr.length; k++) {
                                unichar ch = [phStr characterAtIndex:k];
                                if (ch >= '0' && ch <= '9') [digits appendFormat:@"%C", ch];
                            }
                            if (digits.length) {
                                dict[digits] = name;
                                if (digits.length >= 9) {
                                    dict[[digits substringFromIndex:digits.length - 9]] = name;
                                }
                            }
                            CFRelease(phVal);
                        }
                    }
                    CFRelease(phones);
                }
            }
            CFRelease(people);
        }
        CFRelease(ab);
        s_abCache = [dict copy];
    }
}

NSString *WAAddressBookNameForNumber(NSString *number)
{
    if (!number.length) return nil;
    NSMutableString *digitsOnly = [NSMutableString string];
    for (NSUInteger i = 0; i < number.length; i++) {
        unichar c = [number characterAtIndex:i];
        if (c >= '0' && c <= '9') [digitsOnly appendFormat:@"%C", c];
    }
    if (!digitsOnly.length) return nil;

    ensureAddressBookCache();
    NSString *match = s_abCache[digitsOnly];
    if (match) return match;
    if (digitsOnly.length >= 9) {
        match = s_abCache[[digitsOnly substringFromIndex:digitsOnly.length - 9]];
        if (match) return match;
    }
    return nil;
}

BOOL WAIsRawNumberOrEmpty(NSString *name)
{
    if (!name || ![name isKindOfClass:[NSString class]]) return YES;
    NSString *trimmed = [name stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (!trimmed.length) return YES;
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"0123456789+- ()"];
    if ([[trimmed stringByTrimmingCharactersInSet:allowed] length] == 0) {
        return YES;
    }
    return NO;
}

NSString *WAFormatPhoneNumber(NSString *raw)
{
    if (!raw.length) return @"";
    NSString *user = [raw componentsSeparatedByString:@"@"].firstObject ?: raw;
    NSMutableString *digits = [NSMutableString string];
    for (NSUInteger i = 0; i < user.length; i++) {
        unichar c = [user characterAtIndex:i];
        if (c >= '0' && c <= '9') [digits appendFormat:@"%C", c];
    }
    if (!digits.length) return raw;

    // Czech (+420) / Slovak (+421)
    if (digits.length == 12 && ([digits hasPrefix:@"420"] || [digits hasPrefix:@"421"])) {
        NSString *cc = [digits substringToIndex:3];
        NSString *p1 = [digits substringWithRange:NSMakeRange(3, 3)];
        NSString *p2 = [digits substringWithRange:NSMakeRange(6, 3)];
        NSString *p3 = [digits substringWithRange:NSMakeRange(9, 3)];
        return [NSString stringWithFormat:@"+%@ %@ %@ %@", cc, p1, p2, p3];
    }
    // 9 digits (local Czech/Slovak number without CC)
    if (digits.length == 9) {
        NSString *p1 = [digits substringWithRange:NSMakeRange(0, 3)];
        NSString *p2 = [digits substringWithRange:NSMakeRange(3, 3)];
        NSString *p3 = [digits substringWithRange:NSMakeRange(6, 3)];
        return [NSString stringWithFormat:@"+420 %@ %@ %@", p1, p2, p3];
    }
    // UK (+44)
    if ([digits hasPrefix:@"44"] && (digits.length == 12 || digits.length == 11)) {
        NSString *cc = [digits substringToIndex:2];
        NSString *p1 = [digits substringWithRange:NSMakeRange(2, 4)];
        NSString *p2 = [digits substringFromIndex:6];
        return [NSString stringWithFormat:@"+%@ %@ %@", cc, p1, p2];
    }
    // Spain (+34)
    if ([digits hasPrefix:@"34"] && digits.length == 11) {
        NSString *cc = [digits substringToIndex:2];
        NSString *p1 = [digits substringWithRange:NSMakeRange(2, 3)];
        NSString *p2 = [digits substringWithRange:NSMakeRange(5, 3)];
        NSString *p3 = [digits substringWithRange:NSMakeRange(8, 3)];
        return [NSString stringWithFormat:@"+%@ %@ %@ %@", cc, p1, p2, p3];
    }
    // US/Canada (+1)
    if ([digits hasPrefix:@"1"] && digits.length == 11) {
        NSString *area = [digits substringWithRange:NSMakeRange(1, 3)];
        NSString *p1 = [digits substringWithRange:NSMakeRange(4, 3)];
        NSString *p2 = [digits substringFromIndex:7];
        return [NSString stringWithFormat:@"+1 (%@) %@-%@", area, p1, p2];
    }
    // LID (14-16 digits with @lid)
    if ([raw rangeOfString:@"@lid"].location != NSNotFound || digits.length >= 14) {
        return WALoc(@"contact_whatsapp");
    }
    // General international phone number (starts with country code, 10..13 digits)
    if (digits.length >= 10 && digits.length <= 13) {
        return [NSString stringWithFormat:@"+%@ %@", [digits substringToIndex:3], [digits substringFromIndex:3]];
    }
    return [NSString stringWithFormat:@"+%@", digits];
}

NSString *WADisplayNameForChat(NSString *jid, NSString *savedName)
{
    // 1. If savedName is present and is a genuine human name (not empty, not raw number), use it!
    if (savedName.length && !WAIsRawNumberOrEmpty(savedName)) {
        return savedName;
    }

    // 2. If it's a group (@g.us), return savedName if present, else default
    if ([jid rangeOfString:@"@g.us"].location != NSNotFound) {
        return (savedName.length && !WAIsRawNumberOrEmpty(savedName)) ? savedName : WALoc(@"group_default");
    }

    // 3. Check AddressBook by phone number digits
    NSString *user = [jid componentsSeparatedByString:@"@"].firstObject ?: jid;
    NSString *ab = WAAddressBookNameForNumber(user);
    if (ab.length) return ab;

    // 4. Format as phone number (or WhatsApp kontakt for LID)
    return WAFormatPhoneNumber(jid);
}

// Route the C core's log sink into NSLog (never logs secrets).
static void wa_log_to_nslog(const char *line)
{
    NSLog(@"[wa-core] %s", line ? line : "");
}

__attribute__((constructor)) static void wa_install_log_sink(void)
{
    wa_set_log_sink(wa_log_to_nslog);
}
