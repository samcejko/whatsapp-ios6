// WhatsApp's STUN flavour for the media relay handshake: ALLOCATE / BINDING requests carrying a RELAY-TOKEN,
// MESSAGE-INTEGRITY (HMAC-SHA1) and FINGERPRINT (CRC-32), plus the WhatsApp keepalive ping/pong and the helpers
// for relay-endpoint and sender-subscription attributes. Verified against the meowcaller / whatsapp-rust vectors.
#ifndef WA_WSTUN_H
#define WA_WSTUN_H

#include <stdint.h>
#include <stddef.h>
#include "wa_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WA_STUN_ALLOCATE_REQUEST 0x0003
#define WA_STUN_BINDING_REQUEST  0x0001
#define WA_STUN_ALLOCATE_SUCCESS 0x0103
#define WA_STUN_BINDING_SUCCESS  0x0101
#define WA_STUN_ALLOCATE_ERROR   0x0113
#define WA_STUN_PING             0x0801
#define WA_STUN_PONG             0x0802

#define WA_STUN_ATTR_MAPPED_ADDRESS     0x0001
#define WA_STUN_ATTR_XOR_MAPPED_ADDRESS 0x0020
#define WA_STUN_ATTR_MESSAGE_INTEGRITY  0x0008
#define WA_STUN_ATTR_FINGERPRINT        0x8028
#define WA_STUN_ATTR_RELAY_TOKEN        0x4000
#define WA_STUN_ATTR_ERROR_CODE         0x0009

// CRC-32 (IEEE, reflected) used by the STUN FINGERPRINT.
uint32_t wa_stun_crc32(const uint8_t *data, size_t len);

// Append one STUN attribute (type, length, value, 4-byte padding) to b.
int wa_stun_put_attr(wa_buf *b, uint16_t type, const uint8_t *value, size_t vlen);

// Build a STUN request into out: 20-byte header + attrs, then optional MESSAGE-INTEGRITY (mi_key != NULL) and
// FINGERPRINT (fingerprint != 0). Returns 0 or -1.
int wa_stun_encode_request(wa_buf *out, uint16_t msg_type, const uint8_t tid[12], const uint8_t *attrs, size_t attrs_len,
                           const uint8_t *mi_key, size_t mi_key_len, int fingerprint);
// The 20-byte WhatsApp keepalive ping (type 0x0801, no attrs).
void wa_stun_ping(uint8_t out[20], const uint8_t tid[12]);

// XOR-encoded IPv4:port (6 bytes) for the WASM relay endpoint attribute.
int wa_stun_xor_endpoint(const char *ipv4, uint16_t port, uint8_t out[6]);
// Native sender subscription: 1-byte count=1 + big-endian SSRC (5 bytes).
void wa_stun_native_sender_sub(uint32_t ssrc, uint8_t out[5]);

// Build a WhatsApp Web/WASM STUN ALLOCATE request into out carrying the relay token, stream descriptors, and XOR endpoint.
int wa_stun_build_wasm_allocate(wa_buf *out, const uint8_t tid[12],
                                const uint8_t *token, size_t token_len,
                                const char *ip, uint16_t port,
                                const uint8_t *mi_key, size_t mi_key_len);

// Parsing
int wa_stun_is_stun(const uint8_t *data, size_t len);
int wa_stun_type(const uint8_t *data, size_t len, uint16_t *type);
const uint8_t *wa_stun_tid(const uint8_t *data, size_t len);     // 12 bytes, or NULL
// Finds an attribute value by type (pointer into data). Returns 1 when found.
int wa_stun_find_attr(const uint8_t *data, size_t len, uint16_t type, const uint8_t **val, size_t *vlen);
// Decodes a MAPPED-ADDRESS / XOR-MAPPED-ADDRESS attribute into host-order ip + port (IPv4 only). 1 on success.
int wa_stun_mapped_address(const uint8_t *data, size_t len, uint32_t *ip, uint16_t *port);

int wa_wstun_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
