// Small helpers shared by the whole core: a growable byte buffer, base64/hex, logging, time and randomness.
// Plain C99, the same code builds for iOS 6 (armv7), iOS 14+ (arm64) and the Windows desktop prototype.
#ifndef WA_UTIL_H
#define WA_UTIL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;
    size_t len, cap;
} wa_buf;

void wa_buf_init(wa_buf *b);
void wa_buf_free(wa_buf *b);
void wa_buf_clear(wa_buf *b);                       // keeps the allocation
int wa_buf_reserve(wa_buf *b, size_t extra);         // 0 or -1 (out of memory)
int wa_buf_append(wa_buf *b, const void *data, size_t len);
int wa_buf_append_byte(wa_buf *b, uint8_t v);
int wa_buf_append_str(wa_buf *b, const char *s);
void wa_buf_consume(wa_buf *b, size_t n);            // drops n bytes from the front
uint8_t *wa_buf_detach(wa_buf *b, size_t *len);      // hands the memory to the caller

// Logging: the platform installs a sink (printf on the desktop, the system log on iOS). Never log secrets.
typedef void (*wa_log_sink)(const char *line);
void wa_set_log_sink(wa_log_sink sink);
void wa_log(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void wa_log_hex(const char *label, const uint8_t *data, size_t len);  // debug only: first 64 bytes

// Monotonic microseconds and wall-clock seconds/milliseconds
uint64_t wa_now_us(void);
int64_t wa_unix_time(void);
int64_t wa_unix_ms(void);
void wa_sleep_ms(int ms);

// Cryptographically secure random bytes (the platform's generator). 0 or -1.
int wa_random(void *out, size_t len);
uint32_t wa_random_u32(void);

// base64 (standard alphabet, with padding). Return the output length (without the NUL), or 0 on bad input.
size_t wa_base64_encode(const uint8_t *in, size_t len, char *out, size_t cap);
size_t wa_base64_decode(const char *in, size_t len, uint8_t *out, size_t cap);
// Lower/upper case hex. Returns 2*len.
size_t wa_hex_encode(const uint8_t *in, size_t len, char *out, int upper);
int wa_hex_decode(const char *in, uint8_t *out, size_t cap);   // returns bytes written or -1

char *wa_strdup(const char *s);
char *wa_strndup(const char *s, size_t n);
char *wa_sprintf(const char *fmt, ...)                         // malloc'd, NULL on failure
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Big-endian helpers
static inline void wa_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wa_put_u24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
static inline void wa_put_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline uint16_t wa_get_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t wa_get_u24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static inline uint32_t wa_get_u32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// Constant-time comparison: 0 when equal
int wa_ct_compare(const void *a, const void *b, size_t len);
void wa_wipe(void *p, size_t len);

// Inflate a zlib stream (RFC 1950, zlib header) into out. Appends. Caps the output at 16 MiB. Returns 0 or -1.
int wa_zlib_inflate(const uint8_t *in, size_t len, wa_buf *out);

#ifdef __cplusplus
}
#endif
#endif
