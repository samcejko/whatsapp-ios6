#include "wa_proto.h"

#include <string.h>
#include <stdlib.h>

int pb_put_varint_raw(wa_buf *b, uint64_t v)
{
    uint8_t tmp[10];
    int n = 0;
    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) byte |= 0x80;
        tmp[n++] = byte;
    } while (v);
    return wa_buf_append(b, tmp, (size_t)n);
}

static int put_key(wa_buf *b, uint32_t field, int wire)
{
    return pb_put_varint_raw(b, ((uint64_t)field << 3) | (uint64_t)wire);
}

int pb_put_varint(wa_buf *b, uint32_t field, uint64_t v)
{
    if (put_key(b, field, PB_VARINT) != 0) return -1;
    return pb_put_varint_raw(b, v);
}

int pb_put_bool(wa_buf *b, uint32_t field, int v) { return pb_put_varint(b, field, v ? 1 : 0); }

int pb_put_bytes(wa_buf *b, uint32_t field, const void *data, size_t len)
{
    if (put_key(b, field, PB_BYTES) != 0 || pb_put_varint_raw(b, len) != 0) return -1;
    return wa_buf_append(b, data, len);
}

int pb_put_string(wa_buf *b, uint32_t field, const char *s)
{
    if (!s) return 0;
    return pb_put_bytes(b, field, s, strlen(s));
}

int pb_put_fixed32(wa_buf *b, uint32_t field, uint32_t v)
{
    uint8_t le[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    if (put_key(b, field, PB_FIXED32) != 0) return -1;
    return wa_buf_append(b, le, 4);
}

int pb_put_fixed64(wa_buf *b, uint32_t field, uint64_t v)
{
    uint8_t le[8];
    for (int i = 0; i < 8; i++) le[i] = (uint8_t)(v >> (8 * i));
    if (put_key(b, field, PB_FIXED64) != 0) return -1;
    return wa_buf_append(b, le, 8);
}

int pb_put_message(wa_buf *b, uint32_t field, const wa_buf *sub)
{
    return pb_put_bytes(b, field, sub->data ? sub->data : (const uint8_t *)"", sub->len);
}

void pb_reader_init(pb_reader *r, const uint8_t *data, size_t len)
{
    r->p = data;
    r->end = data + len;
}

static int read_varint(pb_reader *r, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (r->p < r->end && shift < 64) {
        uint8_t byte = *r->p++;
        v |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) { *out = v; return 0; }
        shift += 7;
    }
    return -1;
}

int pb_next(pb_reader *r, pb_field *f)
{
    if (r->p >= r->end) return 0;
    uint64_t key;
    if (read_varint(r, &key) != 0) return -1;
    f->field = (uint32_t)(key >> 3);
    f->wire = (int)(key & 7);
    f->v = 0;
    f->data = NULL;
    f->len = 0;
    switch (f->wire) {
    case PB_VARINT:
        return read_varint(r, &f->v) == 0 ? 1 : -1;
    case PB_FIXED64:
        if (r->end - r->p < 8) return -1;
        for (int i = 0; i < 8; i++) f->v |= (uint64_t)r->p[i] << (8 * i);
        r->p += 8;
        return 1;
    case PB_FIXED32:
        if (r->end - r->p < 4) return -1;
        for (int i = 0; i < 4; i++) f->v |= (uint64_t)r->p[i] << (8 * i);
        r->p += 4;
        return 1;
    case PB_BYTES: {
        uint64_t len;
        if (read_varint(r, &len) != 0 || len > (uint64_t)(r->end - r->p)) return -1;
        f->data = r->p;
        f->len = (size_t)len;
        r->p += len;
        return 1;
    }
    default:
        return -1;   // groups are not used
    }
}

int pb_get_bytes(const uint8_t *msg, size_t len, uint32_t field, const uint8_t **out, size_t *outlen)
{
    pb_reader r;
    pb_field f;
    pb_reader_init(&r, msg, len);
    while (pb_next(&r, &f) == 1) {
        if (f.field == field && f.wire == PB_BYTES) {
            *out = f.data;
            *outlen = f.len;
            return 1;
        }
    }
    return 0;
}

int pb_get_varint(const uint8_t *msg, size_t len, uint32_t field, uint64_t *out)
{
    pb_reader r;
    pb_field f;
    pb_reader_init(&r, msg, len);
    while (pb_next(&r, &f) == 1) {
        if (f.field == field && f.wire != PB_BYTES) {
            *out = f.v;
            return 1;
        }
    }
    return 0;
}

char *pb_get_string(const uint8_t *msg, size_t len, uint32_t field)
{
    const uint8_t *d;
    size_t n;
    if (!pb_get_bytes(msg, len, field, &d, &n)) return NULL;
    return wa_strndup((const char *)d, n);
}

int pb_get_path(const uint8_t *msg, size_t len, const uint32_t *path, int depth, const uint8_t **out, size_t *outlen)
{
    const uint8_t *cur = msg;
    size_t curlen = len;
    for (int i = 0; i < depth; i++) {
        if (!pb_get_bytes(cur, curlen, path[i], &cur, &curlen)) return 0;
    }
    *out = cur;
    *outlen = curlen;
    return 1;
}
