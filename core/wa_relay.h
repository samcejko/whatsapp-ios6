// Parsing the <relay> node WhatsApp sends in a call's <ack>/<transport>: the relay endpoints (where media goes),
// the relay tokens (for the STUN ALLOCATE), the hop-by-hop SRTP key and the WARP MI tag length.
#ifndef WA_RELAY_H
#define WA_RELAY_H

#include <stdint.h>
#include <stddef.h>
#include "wa_node.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WA_RELAY_MAX_ENDPOINTS 8
#define WA_RELAY_MAX_TOKENS 8
#define WA_RELAY_DEFAULT_PORT 3478

typedef struct {
    char ipv4[16];               // empty when ipv6-only
    char ipv6[40];
    uint16_t port;
    int relay_id;
    char relay_name[32];
    int token_id;                // index into relay_tokens
    int auth_token_id;           // index into auth_tokens
    int is_fna;                  // the caller's uplink relay (prefer for an inbound call)
    uint8_t te2_bytes[6];        // raw ipv4:port (echoed verbatim in relaylatency)
    int has_te2_bytes;
} wa_relay_endpoint;

typedef struct {
    wa_relay_endpoint endpoints[WA_RELAY_MAX_ENDPOINTS];
    int nendpoints;
    uint8_t tokens[WA_RELAY_MAX_TOKENS][64];
    size_t token_len[WA_RELAY_MAX_TOKENS];
    int ntokens;
    uint8_t auth_tokens[WA_RELAY_MAX_TOKENS][64];
    size_t auth_token_len[WA_RELAY_MAX_TOKENS];
    int nauth_tokens;
    uint8_t hbh_key[30];
    int has_hbh_key;
    uint32_t warp_mi_tag_len;    // 0 = absent (default 4)
} wa_relay_data;

// Parse a <relay> node. Returns 1 on success (fields filled), 0 when node is not a usable <relay>.
int wa_relay_parse(const wa_node *relay, wa_relay_data *out);
// Pick the media endpoint: prefer the FNA (caller uplink) for an inbound call, else the first non-FNA, else first.
const wa_relay_endpoint *wa_relay_pick_endpoint(const wa_relay_data *d, int inbound);

int wa_relay_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
