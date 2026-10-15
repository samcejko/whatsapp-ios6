// WhatsApp 1:1 call signaling: building the <call> stanzas (offer / preaccept / accept / transport / terminate /
// reject / relaylatency / mute / heartbeat) and the call-key / SRTP setup. The per-device <enc> call-key
// encryption is Signal-session work (done by the messaging layer); these builders take the ready ciphertext.
#ifndef WA_CALL_H
#define WA_CALL_H

#include <stdint.h>
#include <stddef.h>
#include "wa_node.h"
#include "wa_srtp.h"

#ifdef __cplusplus
extern "C" {
#endif

// A new call id: 16 random bytes as uppercase hex (32 chars). buf must hold >= 33.
void wa_call_new_id(char *buf, size_t cap);
// A fresh 32-byte call key.
int wa_call_new_key(uint8_t call_key[32]);

// One per-device encrypted call key for the <offer>.
typedef struct {
    const char *device_jid;     // "user:device@s.whatsapp.net" / "@lid"
    const uint8_t *ciphertext;
    size_t ciphertext_len;
    const char *enc_type;        // "pkmsg" (fresh Signal session) or "msg"
} wa_call_devkey;

typedef struct {
    const char *call_id;
    const char *to;              // peer JID
    const char *call_creator;    // our JID (phone)
    const wa_call_devkey *keys;
    int nkeys;
    const uint8_t *privacy_token; size_t privacy_token_len;   // NULL = absent
    const uint8_t *device_identity; size_t device_identity_len; // NULL = absent (sent with a pkmsg)
    int video;                   // advertise a video call
} wa_call_offer;

// All builders return a newly allocated <call> node (free with wa_node_free), or NULL on failure.
wa_node *wa_call_build_offer(const wa_call_offer *o);
wa_node *wa_call_build_preaccept(const char *call_id, const char *to, const char *call_creator,
                                 const char *wrapper_id, int video);
wa_node *wa_call_build_accept(const char *call_id, const char *to, const char *call_creator,
                              const char *const *audio_rates, int nrates,
                              const uint8_t *relay_te, size_t relay_te_len, int video);
wa_node *wa_call_build_transport(const char *call_id, const char *to, const char *call_creator,
                                 const uint8_t *relay_te, size_t relay_te_len, const char *msg_type);
wa_node *wa_call_build_terminate(const char *call_id, const char *to, const char *call_creator, const char *reason);
wa_node *wa_call_build_reject(const char *call_id, const char *to, const char *call_creator);
wa_node *wa_call_build_mute(const char *call_id, const char *to, const char *call_creator, int muted);
wa_node *wa_call_build_relaylatency(const char *call_id, const char *to, const char *call_creator,
                                    uint32_t latency_ms, const char *relay_name, const uint8_t *addr, size_t addr_len);
wa_node *wa_call_build_heartbeat(const char *call_id, const char *call_creator, const char *wrapper_id);

// Parse the <call><offer> we receive. Fills the fields it finds (pointers into the node; valid while it lives).
typedef struct {
    const char *call_id;
    const char *call_creator;    // the caller
    const char *from;            // <call from>
    int video;                   // the offer carried a <video>
    const wa_node *enc;          // the <enc> with our call-key ciphertext (inline or under destination/to)
    const wa_node *relay;        // the <relay> node, if present
} wa_call_incoming;
// Returns 1 if node is a <call> carrying an <offer>, else 0.
int wa_call_parse_offer(const wa_node *call, wa_call_incoming *out);

int wa_call_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
