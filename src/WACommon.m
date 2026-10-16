#import "WACommon.h"
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

// Route the C core's log sink into NSLog (never logs secrets).
static void wa_log_to_nslog(const char *line)
{
    NSLog(@"[wa-core] %s", line ? line : "");
}

__attribute__((constructor)) static void wa_install_log_sink(void)
{
    wa_set_log_sink(wa_log_to_nslog);
}
