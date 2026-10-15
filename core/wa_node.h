// WhatsApp's binary XML ("WAP"): a node has a tag, string attributes and either children or a byte/string payload.
// JIDs are decoded to their text form ("123:4@s.whatsapp.net", "@s.whatsapp.net" for the bare server) and
// encoded back from it, so the rest of the code only sees strings.
#ifndef WA_NODE_H
#define WA_NODE_H

#include <stdint.h>
#include <stddef.h>
#include "wa_util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wa_node wa_node;
struct wa_node {
    char *tag;
    int nattrs, attrcap;
    char **keys, **vals;
    wa_node **children;
    int nchildren, childcap;
    uint8_t *content;           // payload (NUL-terminated for convenience, content_len excludes it)
    size_t content_len;
    int has_content;
    int content_is_string;      // encode the payload as a (tokenizable) string instead of raw bytes
};

extern const int wa_single_token_count;
extern const char *const wa_single_tokens[];
extern const char *const wa_double_tokens[4][256];

wa_node *wa_node_new(const char *tag);
void wa_node_free(wa_node *n);
wa_node *wa_node_attr(wa_node *n, const char *key, const char *value);        // replaces an existing key; returns n
wa_node *wa_node_attrf(wa_node *n, const char *key, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
wa_node *wa_node_add(wa_node *parent, wa_node *child);                        // returns child
wa_node *wa_node_add_new(wa_node *parent, const char *tag);                    // new child, returned
wa_node *wa_node_set_bytes(wa_node *n, const void *data, size_t len);
wa_node *wa_node_set_string(wa_node *n, const char *s);
wa_node *wa_node_clone(const wa_node *n);

const char *wa_node_get(const wa_node *n, const char *key);                   // NULL when absent
const char *wa_node_get_or(const wa_node *n, const char *key, const char *fallback);
long long wa_node_get_int(const wa_node *n, const char *key, long long fallback);
wa_node *wa_node_child(const wa_node *n, const char *tag);                    // first direct child with the tag
wa_node *wa_node_find(const wa_node *n, const char *tag);                     // depth-first search below n
int wa_node_tag_is(const wa_node *n, const char *tag);

// Encodes the node with the leading frame flags byte (0 = uncompressed). Returns 0 or -1.
int wa_node_encode(const wa_node *n, wa_buf *out);
// Decodes a node from the frame payload AFTER the flags byte and any decompression. NULL on malformed input.
wa_node *wa_node_decode(const uint8_t *data, size_t len);
// Debug text: <tag a="b">...</tag>; byte payloads shown as their length (or hex when show_bytes).
char *wa_node_to_string(const wa_node *n, int show_bytes);

// JID helpers. A JID string: [user][_agent][:device]@server
typedef struct {
    char user[64];
    char server[32];
    int device;
    int agent;
} wa_jid;

int wa_jid_parse(const char *s, wa_jid *out);            // 0 or -1
// Formats user[:device]@server into out (device omitted when 0). Returns out.
char *wa_jid_format(char *out, size_t cap, const char *user, int device, const char *server);
// The same user and server without the device ("123@s.whatsapp.net")
char *wa_jid_to_non_ad(const char *jid, char *out, size_t cap);
int wa_jid_same_user(const char *a, const char *b);

int wa_node_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
