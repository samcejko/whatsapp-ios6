// Call orchestration: placing a 1:1 call (resolve the peer's devices, encrypt the call key to each, send the
// <offer>) and holding the per-call state the media path needs. Runs on the client thread. The media transport
// (wa_rtc) + SRTP keys are brought up when the relay arrives; that part is exercised on the device.
#ifndef WA_CALLENGINE_H
#define WA_CALLENGINE_H

#include "wa_client.h"
#include "wa_sessions.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char call_id[40];
    char peer[80];        // bare peer JID
    uint8_t call_key[32];
    int video;
    int active;
} wa_call_state;

// Place a 1:1 call. Fills *st (call id + key). Returns 0 (offer sent) or -1.
int wa_callengine_place(wa_client *c, wa_sess_store *sessions, const char *to_user_jid, int video, wa_call_state *st);

// Handle an incoming <call> offer: decrypt the call key into *st (ready to accept). Returns 1 if it was an offer.
int wa_callengine_on_offer(wa_client *c, wa_sess_store *sessions, const wa_node *call, wa_call_state *st);

// Send accept / reject / terminate for a known call.
int wa_callengine_accept(wa_client *c, const wa_call_state *st);
int wa_callengine_reject(wa_client *c, const wa_call_state *st);
int wa_callengine_terminate(wa_client *c, const wa_call_state *st, const char *reason);

#ifdef __cplusplus
}
#endif
#endif
