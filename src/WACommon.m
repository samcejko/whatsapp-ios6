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

// Route the C core's log sink into NSLog (never logs secrets).
static void wa_log_to_nslog(const char *line)
{
    NSLog(@"[wa-core] %s", line ? line : "");
}

__attribute__((constructor)) static void wa_install_log_sink(void)
{
    wa_set_log_sink(wa_log_to_nslog);
}
