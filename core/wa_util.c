#include "wa_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
#endif

// ---------------------------------------------------------------------------------------------------- buffer

void wa_buf_init(wa_buf *b) { b->data = NULL; b->len = b->cap = 0; }

void wa_buf_free(wa_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void wa_buf_clear(wa_buf *b) { b->len = 0; }

int wa_buf_reserve(wa_buf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra) cap *= 2;
    uint8_t *p = (uint8_t *)realloc(b->data, cap);
    if (!p) return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

int wa_buf_append(wa_buf *b, const void *data, size_t len)
{
    if (!len) return 0;
    if (wa_buf_reserve(b, len) != 0) return -1;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

int wa_buf_append_byte(wa_buf *b, uint8_t v) { return wa_buf_append(b, &v, 1); }

int wa_buf_append_str(wa_buf *b, const char *s) { return wa_buf_append(b, s, strlen(s)); }

void wa_buf_consume(wa_buf *b, size_t n)
{
    if (n >= b->len) { b->len = 0; return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

uint8_t *wa_buf_detach(wa_buf *b, size_t *len)
{
    uint8_t *p = b->data;
    if (len) *len = b->len;
    if (!p) p = (uint8_t *)malloc(1);
    b->data = NULL;
    b->len = b->cap = 0;
    return p;
}

// ---------------------------------------------------------------------------------------------------- logging

static wa_log_sink g_sink;

void wa_set_log_sink(wa_log_sink sink) { g_sink = sink; }

void wa_log(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_sink) g_sink(line);
    else fprintf(stderr, "%s\n", line);
}

void wa_log_hex(const char *label, const uint8_t *data, size_t len)
{
    char hex[129];
    size_t n = len > 64 ? 64 : len;
    wa_hex_encode(data, n, hex, 0);
    wa_log("%s (%u bytes): %s%s", label, (unsigned)len, hex, len > n ? "..." : "");
}

// ---------------------------------------------------------------------------------------------------- time

uint64_t wa_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1000000.0 / (double)freq.QuadPart);
#elif defined(__APPLE__)
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom / 1000;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
#endif
}

int64_t wa_unix_ms(void)
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;   // 100 ns since 1601
    return (int64_t)(t / 10000u) - 11644473600000LL;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

int64_t wa_unix_time(void) { return wa_unix_ms() / 1000; }

void wa_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

// ---------------------------------------------------------------------------------------------------- random

int wa_random(void *out, size_t len)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, (PUCHAR)out, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
#else
    static int fd = -1;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (fd < 0) fd = open("/dev/urandom", O_RDONLY);
    int ok = fd >= 0;
    uint8_t *p = (uint8_t *)out;
    while (ok && len) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) { ok = 0; break; }
        p += n;
        len -= (size_t)n;
    }
    pthread_mutex_unlock(&lock);
    return ok ? 0 : -1;
#endif
}

uint32_t wa_random_u32(void)
{
    uint32_t v = 0;
    wa_random(&v, sizeof v);
    return v;
}

// ---------------------------------------------------------------------------------------------------- base64 / hex

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t wa_base64_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    size_t need = (len + 2) / 3 * 4;
    if (cap < need + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < len ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

size_t wa_base64_decode(const char *in, size_t len, uint8_t *out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        int c = (unsigned char)in[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = b64val(c);
        if (v < 0) return 0;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return 0;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

size_t wa_hex_encode(const uint8_t *in, size_t len, char *out, int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * len] = 0;
    return 2 * len;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int wa_hex_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t n = strlen(in);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hexval((unsigned char)in[2 * i]), lo = hexval((unsigned char)in[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(n / 2);
}

// ---------------------------------------------------------------------------------------------------- strings

char *wa_strdup(const char *s)
{
    if (!s) return NULL;
    return wa_strndup(s, strlen(s));
}

char *wa_strndup(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *wa_sprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return NULL;
    char *p = (char *)malloc((size_t)n + 1);
    if (!p) return NULL;
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

int wa_ct_compare(const void *a, const void *b, size_t len)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++) d |= x[i] ^ y[i];
    return d;
}

void wa_wipe(void *p, size_t len)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (len--) *v++ = 0;
}
