// Minimal protobuf wire-format writer and reader. Messages are built and read by field number by the callers
// (no generated code): the WhatsApp schema is huge and we only touch a few dozen fields.
#ifndef WA_PROTO_H
#define WA_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include "wa_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PB_VARINT 0
#define PB_FIXED64 1
#define PB_BYTES 2
#define PB_FIXED32 5

// Writer: append fields to a wa_buf. Return 0 or -1.
int pb_put_varint_raw(wa_buf *b, uint64_t v);
int pb_put_varint(wa_buf *b, uint32_t field, uint64_t v);
int pb_put_bool(wa_buf *b, uint32_t field, int v);
int pb_put_bytes(wa_buf *b, uint32_t field, const void *data, size_t len);
int pb_put_string(wa_buf *b, uint32_t field, const char *s);       // skipped when s is NULL
int pb_put_fixed32(wa_buf *b, uint32_t field, uint32_t v);
int pb_put_fixed64(wa_buf *b, uint32_t field, uint64_t v);
// A nested message whose bytes are in sub
int pb_put_message(wa_buf *b, uint32_t field, const wa_buf *sub);

// Reader
typedef struct {
    const uint8_t *p, *end;
} pb_reader;

typedef struct {
    uint32_t field;
    int wire;
    uint64_t v;                 // varint / fixed value
    const uint8_t *data;        // PB_BYTES payload
    size_t len;
} pb_field;

void pb_reader_init(pb_reader *r, const uint8_t *data, size_t len);
// 1 when a field was read, 0 at the end, -1 on malformed input
int pb_next(pb_reader *r, pb_field *f);

// Convenience lookups (the first occurrence of a field). Return 1 when found.
int pb_get_bytes(const uint8_t *msg, size_t len, uint32_t field, const uint8_t **out, size_t *outlen);
int pb_get_varint(const uint8_t *msg, size_t len, uint32_t field, uint64_t *out);
// Copies a string field into a malloc'd NUL-terminated string (NULL when absent)
char *pb_get_string(const uint8_t *msg, size_t len, uint32_t field);
// Follows a path of nested message fields (e.g. {1, 3}) to a bytes/message field
int pb_get_path(const uint8_t *msg, size_t len, const uint32_t *path, int depth, const uint8_t **out, size_t *outlen);

#ifdef __cplusplus
}
#endif
#endif
