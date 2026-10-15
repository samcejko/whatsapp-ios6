#include "wa_node.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// Wire tags (dictionary version 3)
#define T_LIST_EMPTY 0
#define T_DICT0 236
#define T_DICT3 239
#define T_INTEROP_JID 245
#define T_FB_JID 246
#define T_AD_JID 247
#define T_LIST_8 248
#define T_LIST_16 249
#define T_JID_PAIR 250
#define T_HEX_8 251
#define T_BINARY_8 252
#define T_BINARY_20 253
#define T_BINARY_32 254
#define T_NIBBLE_8 255
#define PACKED_MAX 127
#define MAX_DEPTH 64

// ---------------------------------------------------------------------------------------------------- node tree

wa_node *wa_node_new(const char *tag)
{
    wa_node *n = (wa_node *)calloc(1, sizeof *n);
    if (!n) return NULL;
    n->tag = wa_strdup(tag ? tag : "");
    return n;
}

void wa_node_free(wa_node *n)
{
    if (!n) return;
    free(n->tag);
    for (int i = 0; i < n->nattrs; i++) { free(n->keys[i]); free(n->vals[i]); }
    free(n->keys);
    free(n->vals);
    for (int i = 0; i < n->nchildren; i++) wa_node_free(n->children[i]);
    free(n->children);
    free(n->content);
    free(n);
}

wa_node *wa_node_attr(wa_node *n, const char *key, const char *value)
{
    if (!n || !key || !value) return n;
    for (int i = 0; i < n->nattrs; i++) {
        if (strcmp(n->keys[i], key) == 0) {
            free(n->vals[i]);
            n->vals[i] = wa_strdup(value);
            return n;
        }
    }
    if (n->nattrs == n->attrcap) {
        int cap = n->attrcap ? n->attrcap * 2 : 4;
        char **k = (char **)realloc(n->keys, (size_t)cap * sizeof *k);
        if (!k) return n;
        n->keys = k;
        char **v = (char **)realloc(n->vals, (size_t)cap * sizeof *v);
        if (!v) return n;
        n->vals = v;
        n->attrcap = cap;
    }
    n->keys[n->nattrs] = wa_strdup(key);
    n->vals[n->nattrs] = wa_strdup(value);
    n->nattrs++;
    return n;
}

wa_node *wa_node_attrf(wa_node *n, const char *key, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    return wa_node_attr(n, key, tmp);
}

wa_node *wa_node_add(wa_node *parent, wa_node *child)
{
    if (!parent || !child) return child;
    if (parent->nchildren == parent->childcap) {
        int cap = parent->childcap ? parent->childcap * 2 : 4;
        wa_node **c = (wa_node **)realloc(parent->children, (size_t)cap * sizeof *c);
        if (!c) return child;
        parent->children = c;
        parent->childcap = cap;
    }
    parent->children[parent->nchildren++] = child;
    return child;
}

wa_node *wa_node_add_new(wa_node *parent, const char *tag) { return wa_node_add(parent, wa_node_new(tag)); }

wa_node *wa_node_set_bytes(wa_node *n, const void *data, size_t len)
{
    free(n->content);
    n->content = (uint8_t *)malloc(len + 1);
    if (!n->content) { n->has_content = 0; return n; }
    if (len) memcpy(n->content, data, len);
    n->content[len] = 0;
    n->content_len = len;
    n->has_content = 1;
    n->content_is_string = 0;
    return n;
}

wa_node *wa_node_set_string(wa_node *n, const char *s)
{
    wa_node_set_bytes(n, s, strlen(s));
    n->content_is_string = 1;
    return n;
}

wa_node *wa_node_clone(const wa_node *n)
{
    if (!n) return NULL;
    wa_node *c = wa_node_new(n->tag);
    for (int i = 0; i < n->nattrs; i++) wa_node_attr(c, n->keys[i], n->vals[i]);
    for (int i = 0; i < n->nchildren; i++) wa_node_add(c, wa_node_clone(n->children[i]));
    if (n->has_content) {
        wa_node_set_bytes(c, n->content, n->content_len);
        c->content_is_string = n->content_is_string;
    }
    return c;
}

const char *wa_node_get(const wa_node *n, const char *key)
{
    if (!n) return NULL;
    for (int i = 0; i < n->nattrs; i++) if (strcmp(n->keys[i], key) == 0) return n->vals[i];
    return NULL;
}

const char *wa_node_get_or(const wa_node *n, const char *key, const char *fallback)
{
    const char *v = wa_node_get(n, key);
    return v ? v : fallback;
}

long long wa_node_get_int(const wa_node *n, const char *key, long long fallback)
{
    const char *v = wa_node_get(n, key);
    if (!v || !*v) return fallback;
    char *end;
    long long x = strtoll(v, &end, 10);
    return *end ? fallback : x;
}

wa_node *wa_node_child(const wa_node *n, const char *tag)
{
    if (!n) return NULL;
    for (int i = 0; i < n->nchildren; i++) if (strcmp(n->children[i]->tag, tag) == 0) return n->children[i];
    return NULL;
}

wa_node *wa_node_find(const wa_node *n, const char *tag)
{
    if (!n) return NULL;
    for (int i = 0; i < n->nchildren; i++) {
        if (strcmp(n->children[i]->tag, tag) == 0) return n->children[i];
        wa_node *f = wa_node_find(n->children[i], tag);
        if (f) return f;
    }
    return NULL;
}

int wa_node_tag_is(const wa_node *n, const char *tag) { return n && strcmp(n->tag, tag) == 0; }

// ---------------------------------------------------------------------------------------------------- JIDs

int wa_jid_parse(const char *s, wa_jid *out)
{
    memset(out, 0, sizeof *out);
    const char *at = strchr(s, '@');
    if (!at) {
        if (strlen(s) >= sizeof out->server) return -1;
        strcpy(out->server, s);
        return 0;
    }
    size_t ulen = (size_t)(at - s);
    if (ulen >= sizeof out->user || strlen(at + 1) >= sizeof out->server) return -1;
    strcpy(out->server, at + 1);
    char user[64];
    memcpy(user, s, ulen);
    user[ulen] = 0;
    char *colon = strchr(user, ':');
    if (colon) {
        out->device = atoi(colon + 1);
        *colon = 0;
    }
    char *us = strchr(user, '_');
    if (us && us[1] >= '0' && us[1] <= '9') {
        out->agent = atoi(us + 1);
        *us = 0;
    }
    strcpy(out->user, user);
    return 0;
}

char *wa_jid_format(char *out, size_t cap, const char *user, int device, const char *server)
{
    if (device > 0) snprintf(out, cap, "%s:%d@%s", user ? user : "", device, server);
    else snprintf(out, cap, "%s@%s", user ? user : "", server);
    return out;
}

char *wa_jid_to_non_ad(const char *jid, char *out, size_t cap)
{
    wa_jid j;
    if (wa_jid_parse(jid, &j) != 0) { snprintf(out, cap, "%s", jid); return out; }
    return wa_jid_format(out, cap, j.user, 0, j.server);
}

int wa_jid_same_user(const char *a, const char *b)
{
    wa_jid x, y;
    if (wa_jid_parse(a, &x) != 0 || wa_jid_parse(b, &y) != 0) return 0;
    return strcmp(x.user, y.user) == 0 && strcmp(x.server, y.server) == 0;
}

// ---------------------------------------------------------------------------------------------------- token index

#define TOKEN_HASH_SIZE 4096
typedef struct { const char *s; uint16_t code; } tok_slot;   // code: index (single) or 0x100*(dict+1)+index
static tok_slot g_tokens[TOKEN_HASH_SIZE];
static int g_tokens_ready;

static uint32_t str_hash(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

static void tok_insert(const char *s, uint16_t code)
{
    if (!*s) return;
    uint32_t h = str_hash(s, strlen(s)) & (TOKEN_HASH_SIZE - 1);
    while (g_tokens[h].s) {
        if (strcmp(g_tokens[h].s, s) == 0) return;   // keep the first (single-byte tokens win)
        h = (h + 1) & (TOKEN_HASH_SIZE - 1);
    }
    g_tokens[h].s = s;
    g_tokens[h].code = code;
}

static void tokens_init(void)
{
    if (g_tokens_ready) return;
    for (int i = 1; i < wa_single_token_count; i++) tok_insert(wa_single_tokens[i], (uint16_t)i);
    for (int d = 0; d < 4; d++)
        for (int i = 0; i < 256; i++) tok_insert(wa_double_tokens[d][i], (uint16_t)(0x100 * (d + 1) + i));
    g_tokens_ready = 1;
}

static int tok_lookup(const char *s, size_t len, uint16_t *code)
{
    uint32_t h = str_hash(s, len) & (TOKEN_HASH_SIZE - 1);
    while (g_tokens[h].s) {
        if (strlen(g_tokens[h].s) == len && memcmp(g_tokens[h].s, s, len) == 0) { *code = g_tokens[h].code; return 1; }
        h = (h + 1) & (TOKEN_HASH_SIZE - 1);
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------- encoder

typedef struct {
    wa_buf *b;
    int err;
} enc;

static void put8(enc *e, uint8_t v) { if (wa_buf_append_byte(e->b, v) != 0) e->err = 1; }
static void putn(enc *e, const void *p, size_t n) { if (wa_buf_append(e->b, p, n) != 0) e->err = 1; }

static void write_list_start(enc *e, size_t n)
{
    if (n == 0) put8(e, T_LIST_EMPTY);
    else if (n < 256) { put8(e, T_LIST_8); put8(e, (uint8_t)n); }
    else { uint8_t t[2]; wa_put_u16(t, (uint16_t)n); put8(e, T_LIST_16); putn(e, t, 2); }
}

static void write_bytes_len(enc *e, const uint8_t *p, size_t n)
{
    if (n < 256) { put8(e, T_BINARY_8); put8(e, (uint8_t)n); }
    else if (n < (1u << 20)) { uint8_t t[3]; wa_put_u24(t, (uint32_t)n); put8(e, T_BINARY_20); putn(e, t, 3); }
    else { uint8_t t[4]; wa_put_u32(t, (uint32_t)n); put8(e, T_BINARY_32); putn(e, t, 4); }
    putn(e, p, n);
}

static int nibble_of(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c == '-') return 10;
    if (c == '.') return 11;
    return -1;
}

static int hex_of(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int packable(const char *s, size_t len, int (*fn)(char))
{
    if (len == 0 || len > PACKED_MAX) return 0;
    for (size_t i = 0; i < len; i++) if (fn(s[i]) < 0) return 0;
    return 1;
}

static void write_packed(enc *e, const char *s, size_t len, uint8_t tag, int (*fn)(char))
{
    put8(e, tag);
    uint8_t rounded = (uint8_t)((len + 1) / 2);
    if (len % 2) rounded |= 0x80;
    put8(e, rounded);
    for (size_t i = 0; i < len; i += 2) {
        int hi = fn(s[i]);
        int lo = i + 1 < len ? fn(s[i + 1]) : 15;
        put8(e, (uint8_t)(hi << 4 | lo));
    }
}

static void write_string(enc *e, const char *s, size_t len);

static int server_domain_type(const char *server, int *out)
{
    if (strcmp(server, "s.whatsapp.net") == 0) { *out = 0; return 1; }
    if (strcmp(server, "lid") == 0) { *out = 1; return 1; }
    if (strcmp(server, "hosted") == 0) { *out = 128; return 1; }
    if (strcmp(server, "hosted.lid") == 0) { *out = 129; return 1; }
    return 0;
}

// Returns 1 when s was written as a JID
static int try_write_jid(enc *e, const char *s, size_t len)
{
    const char *at = memchr(s, '@', len);
    if (!at || len > 48) return 0;
    const char *server = at + 1;
    size_t slen = len - (size_t)(server - s);
    size_t ulen = (size_t)(at - s);
    char srv[48];
    if (slen >= sizeof srv) return 0;
    memcpy(srv, server, slen);
    srv[slen] = 0;

    // user[_agent][:device]
    size_t user_end = ulen;
    int device = -1;
    const char *colon = memchr(s, ':', ulen);
    if (colon) {
        const char *d = colon + 1;
        size_t dl = (size_t)(at - d);
        int ok = dl > 0 && dl <= 3, v = 0;
        for (size_t i = 0; ok && i < dl; i++) { if (d[i] < '0' || d[i] > '9') ok = 0; else v = v * 10 + (d[i] - '0'); }
        if (ok && v <= 255) { device = v; user_end = (size_t)(colon - s); }
    }
    const char *us = memchr(s, '_', user_end);
    if (us && us + 1 < s + user_end) {
        int ok = 1;
        for (const char *p = us + 1; p < s + user_end; p++) if (*p < '0' || *p > '9') ok = 0;
        if (ok) user_end = (size_t)(us - s);
    }
    int domain;
    if (device >= 0 && server_domain_type(srv, &domain)) {
        put8(e, T_AD_JID);
        put8(e, (uint8_t)domain);
        put8(e, (uint8_t)device);
        write_string(e, s, user_end);
    } else {
        put8(e, T_JID_PAIR);
        if (user_end == 0) put8(e, T_LIST_EMPTY);
        else write_string(e, s, user_end);
        write_string(e, srv, slen);
    }
    return 1;
}

static void write_string(enc *e, const char *s, size_t len)
{
    if (len == 0) { put8(e, T_BINARY_8); put8(e, 0); return; }
    if (len <= PACKED_MAX) {
        uint16_t code;
        if (tok_lookup(s, len, &code)) {
            if (code < 0x100) put8(e, (uint8_t)code);
            else { put8(e, (uint8_t)(T_DICT0 + (code >> 8) - 1)); put8(e, (uint8_t)(code & 0xFF)); }
            return;
        }
        if (packable(s, len, nibble_of)) { write_packed(e, s, len, T_NIBBLE_8, nibble_of); return; }
        if (packable(s, len, hex_of)) { write_packed(e, s, len, T_HEX_8, hex_of); return; }
        if (try_write_jid(e, s, len)) return;
    }
    write_bytes_len(e, (const uint8_t *)s, len);
}

static void write_node(enc *e, const wa_node *n)
{
    size_t size = 1 + 2 * (size_t)n->nattrs + ((n->nchildren > 0 || n->has_content) ? 1 : 0);
    write_list_start(e, size);
    write_string(e, n->tag, strlen(n->tag));
    for (int i = 0; i < n->nattrs; i++) {
        write_string(e, n->keys[i], strlen(n->keys[i]));
        write_string(e, n->vals[i], strlen(n->vals[i]));
    }
    if (n->nchildren > 0) {
        write_list_start(e, (size_t)n->nchildren);
        for (int i = 0; i < n->nchildren; i++) write_node(e, n->children[i]);
    } else if (n->has_content) {
        if (n->content_is_string) write_string(e, (const char *)n->content, n->content_len);
        else write_bytes_len(e, n->content, n->content_len);
    }
}

int wa_node_encode(const wa_node *n, wa_buf *out)
{
    tokens_init();
    enc e = { out, 0 };
    put8(&e, 0);
    write_node(&e, n);
    return e.err ? -1 : 0;
}

// ---------------------------------------------------------------------------------------------------- decoder

typedef struct {
    const uint8_t *p, *end;
    int err;
} dec;

static int rd8(dec *d)
{
    if (d->p >= d->end) { d->err = 1; return 0; }
    return *d->p++;
}

static uint32_t rdbe(dec *d, int n)
{
    if (d->end - d->p < n) { d->err = 1; return 0; }
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | *d->p++;
    return v;
}

static const uint8_t *rdbytes(dec *d, size_t n)
{
    if ((size_t)(d->end - d->p) < n) { d->err = 1; return NULL; }
    const uint8_t *p = d->p;
    d->p += n;
    return p;
}

static size_t list_size(dec *d, int tag)
{
    if (tag == T_LIST_EMPTY) return 0;
    if (tag == T_LIST_8) return (size_t)rd8(d);
    if (tag == T_LIST_16) return rdbe(d, 2);
    d->err = 1;
    return 0;
}

// Reads a value as a malloc'd string (NULL for LIST_EMPTY). *len gets its length (binary values may hold NULs).
static char *read_string_tag(dec *d, int tag, size_t *len);

static char *read_string(dec *d, size_t *len)
{
    int tag = rd8(d);
    if (d->err) return NULL;
    return read_string_tag(d, tag, len);
}

static char *unpack(dec *d, int tag, size_t *len)
{
    int b = rd8(d);
    size_t n = (size_t)(b & 0x7F);
    const uint8_t *p = rdbytes(d, n);
    if (!p) return NULL;
    char *s = (char *)malloc(n * 2 + 1);
    if (!s) { d->err = 1; return NULL; }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int nib[2] = { p[i] >> 4, p[i] & 15 };
        for (int k = 0; k < 2; k++) {
            int v = nib[k];
            char c;
            if (tag == T_HEX_8) c = (char)(v < 10 ? '0' + v : 'A' + v - 10);
            else if (v < 10) c = (char)('0' + v);
            else if (v == 10) c = '-';
            else if (v == 11) c = '.';
            else if (v == 15) c = 0;
            else { free(s); d->err = 1; return NULL; }
            s[o++] = c;
        }
    }
    if ((b & 0x80) && o > 0) o--;
    s[o] = 0;
    *len = o;
    return s;
}

static char *read_jid_pair(dec *d, size_t *len)
{
    size_t ul = 0, sl = 0;
    char *user = read_string(d, &ul);
    char *server = read_string(d, &sl);
    if (d->err) { free(user); free(server); return NULL; }
    char *s = wa_sprintf("%s@%s", user ? user : "", server ? server : "");
    free(user);
    free(server);
    *len = s ? strlen(s) : 0;
    return s;
}

static char *read_ad_jid(dec *d, size_t *len)
{
    int domain = rd8(d);
    int device = rd8(d);
    size_t ul = 0;
    char *user = read_string(d, &ul);
    if (d->err || !user) { free(user); d->err = 1; return NULL; }
    const char *server;
    if (domain == 0) server = "s.whatsapp.net";
    else if (domain == 1) server = "lid";
    else if (domain == 129) server = "hosted.lid";
    else if (domain & 128) server = "hosted";
    else { free(user); d->err = 1; return NULL; }
    char buf[128];
    wa_jid_format(buf, sizeof buf, user, device, server);
    free(user);
    *len = strlen(buf);
    return wa_strdup(buf);
}

static char *read_other_jid(dec *d, int interop, size_t *len)
{
    size_t ul = 0, sl = 0;
    char *user = read_string(d, &ul);
    int device = (int)rdbe(d, 2);
    int integrator = interop ? (int)rdbe(d, 2) : 0;
    char *server = read_string(d, &sl);
    if (d->err) { free(user); free(server); return NULL; }
    char *s = interop ? wa_sprintf("%s:%d.%d@%s", user ? user : "", device, integrator, server ? server : "interop")
                      : wa_sprintf("%s:%d@%s", user ? user : "", device, server ? server : "msgr");
    free(user);
    free(server);
    *len = s ? strlen(s) : 0;
    return s;
}

static char *read_string_tag(dec *d, int tag, size_t *len)
{
    *len = 0;
    switch (tag) {
    case T_LIST_EMPTY:
        return NULL;
    case T_BINARY_8:
    case T_BINARY_20:
    case T_BINARY_32: {
        size_t n = tag == T_BINARY_8 ? (size_t)rd8(d) : tag == T_BINARY_20 ? (rdbe(d, 3) & 0xFFFFF) : rdbe(d, 4);
        const uint8_t *p = rdbytes(d, n);
        if (!p) return NULL;
        *len = n;
        return wa_strndup((const char *)p, n);
    }
    case T_JID_PAIR:
        return read_jid_pair(d, len);
    case T_AD_JID:
        return read_ad_jid(d, len);
    case T_INTEROP_JID:
        return read_other_jid(d, 1, len);
    case T_FB_JID:
        return read_other_jid(d, 0, len);
    case T_NIBBLE_8:
    case T_HEX_8:
        return unpack(d, tag, len);
    default:
        if (tag >= T_DICT0 && tag <= T_DICT3) {
            int idx = rd8(d);
            if (d->err) return NULL;
            const char *s = wa_double_tokens[tag - T_DICT0][idx];
            *len = strlen(s);
            return wa_strdup(s);
        }
        if (tag < wa_single_token_count) {
            const char *s = wa_single_tokens[tag];
            *len = strlen(s);
            return wa_strdup(s);
        }
        d->err = 1;
        return NULL;
    }
}

static wa_node *read_node(dec *d, int depth)
{
    if (depth > MAX_DEPTH) { d->err = 1; return NULL; }
    size_t size = list_size(d, rd8(d));
    if (d->err || size == 0) { d->err = 1; return NULL; }
    size_t tl = 0;
    char *tag = read_string(d, &tl);
    if (d->err || !tag) { free(tag); d->err = 1; return NULL; }
    wa_node *n = wa_node_new(tag);
    free(tag);
    size_t nattrs = (size - 1) / 2;
    for (size_t i = 0; i < nattrs && !d->err; i++) {
        size_t kl = 0, vl = 0;
        char *k = read_string(d, &kl);
        char *v = read_string(d, &vl);
        if (k) wa_node_attr(n, k, v ? v : "");
        else d->err = 1;
        free(k);
        free(v);
    }
    if (!d->err && size % 2 == 0) {
        int ct = rd8(d);
        if (ct == T_LIST_8 || ct == T_LIST_16) {
            size_t count = list_size(d, ct);
            for (size_t i = 0; i < count && !d->err; i++) {
                wa_node *c = read_node(d, depth + 1);
                if (c) wa_node_add(n, c);
            }
        } else if (ct == T_BINARY_8 || ct == T_BINARY_20 || ct == T_BINARY_32) {
            size_t len = ct == T_BINARY_8 ? (size_t)rd8(d) : ct == T_BINARY_20 ? (rdbe(d, 3) & 0xFFFFF) : rdbe(d, 4);
            const uint8_t *p = rdbytes(d, len);
            if (p) wa_node_set_bytes(n, p, len);
        } else if (ct != T_LIST_EMPTY && !d->err) {
            size_t len = 0;
            char *s = read_string_tag(d, ct, &len);
            if (s) {
                wa_node_set_bytes(n, s, len);
                n->content_is_string = 1;
                free(s);
            }
        }
    }
    if (d->err) { wa_node_free(n); return NULL; }
    return n;
}

wa_node *wa_node_decode(const uint8_t *data, size_t len)
{
    dec d = { data, data + len, 0 };
    wa_node *n = read_node(&d, 0);
    if (d.err) { wa_node_free(n); return NULL; }
    return n;
}

// ---------------------------------------------------------------------------------------------------- debug text

static void to_string(const wa_node *n, wa_buf *b, int show_bytes, int indent)
{
    for (int i = 0; i < indent; i++) wa_buf_append_str(b, "  ");
    wa_buf_append_byte(b, '<');
    wa_buf_append_str(b, n->tag);
    for (int i = 0; i < n->nattrs; i++) {
        wa_buf_append_byte(b, ' ');
        wa_buf_append_str(b, n->keys[i]);
        wa_buf_append_str(b, "=\"");
        wa_buf_append_str(b, n->vals[i]);
        wa_buf_append_byte(b, '"');
    }
    if (!n->nchildren && !n->has_content) { wa_buf_append_str(b, "/>\n"); return; }
    wa_buf_append_byte(b, '>');
    if (n->nchildren) {
        wa_buf_append_byte(b, '\n');
        for (int i = 0; i < n->nchildren; i++) to_string(n->children[i], b, show_bytes, indent + 1);
        for (int i = 0; i < indent; i++) wa_buf_append_str(b, "  ");
    } else {
        int printable = n->content_len < 200;
        for (size_t i = 0; i < n->content_len && printable; i++) if (n->content[i] < 0x20 || n->content[i] > 0x7e) printable = 0;
        if (printable) wa_buf_append(b, n->content, n->content_len);
        else if (show_bytes) {
            size_t m = n->content_len > 96 ? 96 : n->content_len;
            char *hex = (char *)malloc(m * 2 + 1);
            if (hex) { wa_hex_encode(n->content, m, hex, 0); wa_buf_append_str(b, hex); free(hex); }
            if (m < n->content_len) wa_buf_append_str(b, "...");
        } else {
            char tmp[48];
            snprintf(tmp, sizeof tmp, "[%u bytes]", (unsigned)n->content_len);
            wa_buf_append_str(b, tmp);
        }
    }
    wa_buf_append_str(b, "</");
    wa_buf_append_str(b, n->tag);
    wa_buf_append_str(b, ">\n");
}

char *wa_node_to_string(const wa_node *n, int show_bytes)
{
    wa_buf b;
    wa_buf_init(&b);
    if (n) to_string(n, &b, show_bytes, 0);
    wa_buf_append_byte(&b, 0);
    return (char *)wa_buf_detach(&b, NULL);
}

// ---------------------------------------------------------------------------------------------------- self test

int wa_node_selftest(void)
{
    wa_node *iq = wa_node_new("iq");
    wa_node_attr(iq, "id", "3EB0A1B2C3D4E5F6");
    wa_node_attr(iq, "to", "@s.whatsapp.net");
    wa_node_attr(iq, "type", "set");
    wa_node_attr(iq, "xmlns", "md");
    wa_node_attr(iq, "target", "4915112345678:12@s.whatsapp.net");
    wa_node_attr(iq, "lidj", "123456789012345@lid");
    wa_node_attr(iq, "num", "1.5-2");
    wa_node_attr(iq, "free", "hello world");
    wa_node *c = wa_node_add_new(iq, "pair-device-sign");
    wa_node *di = wa_node_add_new(c, "device-identity");
    wa_node_attr(di, "key-index", "3");
    uint8_t blob[300];
    for (int i = 0; i < 300; i++) blob[i] = (uint8_t)i;
    wa_node_set_bytes(di, blob, sizeof blob);

    wa_buf b;
    wa_buf_init(&b);
    int ok = wa_node_encode(iq, &b) == 0 && b.len > 1 && b.data[0] == 0;
    wa_node *back = ok ? wa_node_decode(b.data + 1, b.len - 1) : NULL;
    wa_buf b2;
    wa_buf_init(&b2);
    if (back) ok = wa_node_encode(back, &b2) == 0 && b2.len == b.len && memcmp(b.data, b2.data, b.len) == 0;
    else ok = 0;
    if (ok) {
        ok = strcmp(wa_node_get_or(back, "to", ""), "@s.whatsapp.net") == 0 &&
             strcmp(wa_node_get_or(back, "target", ""), "4915112345678:12@s.whatsapp.net") == 0 &&
             strcmp(wa_node_get_or(back, "lidj", ""), "123456789012345@lid") == 0 &&
             strcmp(wa_node_get_or(back, "num", ""), "1.5-2") == 0 &&
             strcmp(wa_node_get_or(back, "id", ""), "3EB0A1B2C3D4E5F6") == 0;
        wa_node *d2 = wa_node_find(back, "device-identity");
        ok = ok && d2 && d2->content_len == 300 && memcmp(d2->content, blob, 300) == 0;
    }
    if (!ok) {
        char *s = back ? wa_node_to_string(back, 1) : NULL;
        wa_log("selftest: node round trip failed%s%s", s ? "\n" : "", s ? s : "");
        free(s);
    }
    wa_buf_free(&b);
    wa_buf_free(&b2);
    wa_node_free(iq);
    wa_node_free(back);
    return ok ? 0 : -1;
}
