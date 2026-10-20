// Call orchestration: placing a 1:1 call (resolve the peer's devices, encrypt the call key to each, send the
// <offer>) and holding the per-call state the media path needs. Runs on the client thread. The media transport
// (wa_rtc) + SRTP keys are brought up when the relay arrives; that part is exercised on the device.
#ifndef WA_CALLENGINE_H
#define WA_CALLENGINE_H

#include "wa_client.h"
#include "wa_sessions.h"
#include "wa_relay.h"
#include "wa_srtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char call_id[40];
    char peer[80];        // bare peer JID
    uint8_t call_key[32];
    int video;
    int active;
    int is_incoming;      // 1 = incoming call, 0 = outgoing call (for correct relay endpoint selection)
    wa_relay_data relay;
    int has_relay;
    wa_srtp_keys send_keys;
    wa_srtp_keys recv_keys;
    uint32_t send_ssrc;
    uint32_t recv_ssrc;
    wa_roc_send roc_send;
    wa_roc_recv roc_recv;
    uint16_t rtp_seq;
    uint32_t rtp_ts;
    int srtp_ready;
} wa_call_state;

// Place a 1:1 call. Fills *st (call id + key). Returns 0 (offer sent) or -1.
int wa_callengine_place(wa_client *c, wa_sess_store *sessions, const char *to_user_jid, int video, wa_call_state *st);

// Handle an incoming <call> offer: decrypt the call key into *st (ready to accept). Returns 1 if it was an offer.
int wa_callengine_on_offer(wa_client *c, wa_sess_store *sessions, const wa_node *call, wa_call_state *st);

// Parse relay information from an incoming <call> stanza (which may contain <relay>, <relaylatency>, or <transport>)
int wa_callengine_on_relay(const wa_node *call, wa_call_state *st);

// Derive SRTP keys and SSRCs from call_key, self_lid, and peer_lid
int wa_callengine_setup_srtp(wa_call_state *st, const char *self_lid, const char *peer_lid);

// Send accept / reject / terminate for a known call.
int wa_callengine_accept(wa_client *c, const wa_call_state *st);
int wa_callengine_reject(wa_client *c, const wa_call_state *st);
int wa_callengine_terminate(wa_client *c, const wa_call_state *st, const char *reason);

#ifdef __cplusplus
}
#endif
#endif
