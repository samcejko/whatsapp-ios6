// WhatsApp call media crypto (E2E 1:1 path): SRTP session keys derived from the 32-byte call key + a participant
// LID via HKDF-SHA256 and an AES-CM PRF, AES-128-CTR payload encryption, and the 4-byte WARP MESSAGE-INTEGRITY tag
// (HMAC-SHA1) that authenticates each relayed packet. Verified against the meowcaller/whatsapp-rust KAT vectors.
#ifndef WA_SRTP_H
#define WA_SRTP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WA_WARP_TAG_LEN 4

typedef struct {
    uint8_t cipher_key[16];
    uint8_t salt[14];
    uint8_t auth_key[20];
} wa_srtp_keys;

// Derive the E2E 1:1 SRTP keys from call_key (>=32 bytes) and a participant LID string (used as HKDF info).
// The sender derives SEND keys from its own LID and RECV keys from the peer's LID. 0 or -1 (call_key too short).
int wa_srtp_derive_e2e(wa_srtp_keys *out, const uint8_t *call_key, size_t call_key_len, const char *participant_lid);
// The matching SRTCP keys (different KDF labels 0x03/0x04/0x05).
int wa_srtp_derive_e2e_rtcp(wa_srtp_keys *out, const uint8_t *call_key, size_t call_key_len, const char *participant_lid);

// Build the AES-CTR IV for one RTP packet (salt ^ ssrc ^ 48-bit packet index).
void wa_srtp_rtp_iv(const wa_srtp_keys *k, uint32_t ssrc, uint32_t roc, uint16_t seq, uint8_t iv[16]);
// Encrypt/decrypt an RTP payload in place (AES-128-CTR; symmetric). 0 or -1.
int wa_srtp_crypt_payload(const wa_srtp_keys *k, uint32_t ssrc, uint32_t roc, uint16_t seq, uint8_t *data, size_t len);

// WARP MI tag = first 4 bytes of HMAC-SHA1(auth_key, packet || roc_be32). Writes WA_WARP_TAG_LEN bytes to tag.
void wa_srtp_warp_tag(const uint8_t auth_key[20], const uint8_t *packet, size_t len, uint32_t roc, uint8_t tag[WA_WARP_TAG_LEN]);
// 0 when the received tag (tag_len bytes) matches, -1 otherwise (constant-time over the compared length).
int wa_srtp_warp_verify(const uint8_t auth_key[20], const uint8_t *packet, size_t len, uint32_t roc,
                        const uint8_t *received_tag, size_t tag_len);

// Send-side ROC: monotonic 16-bit sequence rollover counter.
typedef struct { uint32_t roc; uint16_t last_seq; int started; } wa_roc_send;
uint32_t wa_roc_send_advance(wa_roc_send *t, uint16_t seq);

// Recv-side ROC: RFC 3711 guess-index. estimate without mutating; commit only after the tag verifies.
typedef struct { uint32_t roc; uint16_t s_l; int started; } wa_roc_recv;
uint32_t wa_roc_recv_estimate(const wa_roc_recv *t, uint16_t seq);
void wa_roc_recv_commit(wa_roc_recv *t, uint32_t v, uint16_t seq);

int wa_srtp_selftest(void);   // 0 when the KAT vectors pass

#ifdef __cplusplus
}
#endif
#endif
