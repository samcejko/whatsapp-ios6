// The Signal protocol (X3DH + Double Ratchet) as WhatsApp uses it, in C: 1:1 session establishment from a peer's
// prekey bundle, and message encrypt/decrypt (SignalMessage / PreKeySignalMessage). This is what encrypts the
// per-device call key in a call <offer>/<enc> and every 1:1 text message. Curve25519 identity keys, AES-256-CBC
// payloads, HMAC-SHA256 truncated MAC — all per libsignal.
#ifndef WA_SIGNAL_H
#define WA_SIGNAL_H

#include <stdint.h>
#include <stddef.h>
#include "wa_util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint8_t priv[32], pub[32]; } wa_sig_keypair;

// A peer device's prekey bundle (fetched via the prekey <iq>).
typedef struct {
    uint32_t registration_id;
    uint8_t identity_pub[32];
    uint32_t signed_prekey_id;
    uint8_t signed_prekey_pub[32];
    uint8_t signed_prekey_sig[64];
    int has_one_time;
    uint32_t prekey_id;
    uint8_t one_time_pub[32];
} wa_sig_bundle;

#define WA_SIG_MAX_RECV_CHAINS 5
#define WA_SIG_MAX_SKIPPED 320

typedef struct { uint8_t ratchet[32]; uint32_t counter; uint8_t cipher[32], mac[32], iv[16]; } wa_sig_skipped;
typedef struct { uint8_t ratchet[32], chain[32]; uint32_t index; int used; } wa_sig_recv_chain;

typedef struct {
    int established;
    uint8_t version;
    uint8_t local_identity[32], remote_identity[32];
    uint8_t root[32];
    // sending chain
    wa_sig_keypair send_ratchet;
    uint8_t send_chain[32];
    uint32_t send_index;
    uint32_t previous_counter;
    int has_send;
    // receiving chains (current + a few previous, newest first)
    wa_sig_recv_chain recv[WA_SIG_MAX_RECV_CHAINS];
    int nrecv;
    // skipped message keys (ring)
    wa_sig_skipped skipped[WA_SIG_MAX_SKIPPED];
    int nskipped;
    // pending prekey info — present until the peer answers; makes our next message a PreKeySignalMessage
    int has_pending;
    uint32_t pending_reg_id;
    int pending_has_prekey;
    uint32_t pending_prekey_id;
    uint32_t pending_signed_prekey_id;
    uint8_t pending_base_pub[32];
} wa_sig_session;

// Alice side: build an outgoing session from a peer bundle (verifies the signed-prekey signature). 0 or -1.
int wa_sig_init_outgoing(wa_sig_session *s, const wa_sig_keypair *our_identity, uint32_t our_reg_id, const wa_sig_bundle *b);

// Encrypt plaintext. Sets *is_prekey (1 = the output is a PreKeySignalMessage / "pkmsg", else a SignalMessage /
// "msg"). Appends the wire bytes to out. 0 or -1.
int wa_sig_encrypt(wa_sig_session *s, const wa_sig_keypair *our_identity, const uint8_t *pt, size_t ptlen,
                   int *is_prekey, wa_buf *out);

// Read the ids out of a received PreKeySignalMessage so the caller can look up the right prekeys. 0 or -1.
int wa_sig_prekey_header(const uint8_t *msg, size_t len, uint32_t *reg_id, int *has_prekey, uint32_t *prekey_id,
                         uint32_t *signed_prekey_id, uint8_t base_pub[32], uint8_t identity_pub[32]);

// Bob side: establish a session from a received PreKeySignalMessage and decrypt it. our_one_time may be NULL.
int wa_sig_decrypt_prekey(wa_sig_session *s, const wa_sig_keypair *our_identity,
                          const wa_sig_keypair *our_signed_prekey, const wa_sig_keypair *our_one_time,
                          const uint8_t *msg, size_t len, wa_buf *out);

// Decrypt a normal SignalMessage on an established session. 0 or -1 (bad MAC / no key).
int wa_sig_decrypt(wa_sig_session *s, const uint8_t *msg, size_t len, wa_buf *out);

// WhatsApp message padding (random 1..16 pad byte, value = pad length, repeated) around the Signal plaintext.
int wa_sig_pad(const uint8_t *in, size_t len, wa_buf *out);
int wa_sig_unpad(const uint8_t *in, size_t len, wa_buf *out);

int wa_signal_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
