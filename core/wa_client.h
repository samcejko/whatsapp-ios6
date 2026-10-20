// The WhatsApp linked-device connection driver: opens the WebSocket, runs the Noise handshake with the client
// payload, encrypts/decrypts binary-node stanzas, and runs the QR pairing exchange. Single owner thread: it calls
// wa_client_connect once, then wa_client_run (or repeatedly wa_client_poll). Callbacks fire on that thread.
#ifndef WA_CLIENT_H
#define WA_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include "wa_store.h"
#include "wa_node.h"
#include "wa_fallback.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wa_client wa_client;

typedef enum {
    WA_DISCONNECTED = 0,
    WA_CONNECTING,
    WA_HANDSHAKING,
    WA_PAIRING,          // showing a QR, waiting for the phone
    WA_CONNECTED,        // logged in
} wa_client_state;

typedef struct {
    void *ctx;
    // The QR string to display (render as a QR code). Called again when it should refresh (ref rotation).
    void (*on_qr)(void *ctx, const char *qr);
    // Pairing finished: our JID and push name are now set in the device; a reconnect follows automatically.
    void (*on_paired)(void *ctx, const char *jid);
    // Logged in and ready.
    void (*on_connected)(void *ctx);
    // A stanza arrived that the driver did not handle itself (messages, receipts, calls, iq results...).
    void (*on_node)(void *ctx, const wa_node *node);
    // The connection ended. reconnect is 1 for an expected reconnect (e.g. post-pair 515), 0 for a real failure.
    void (*on_disconnect)(void *ctx, const char *reason, int reconnect);
    // Optional: a place to persist the device after pairing changed it.
    void (*on_device_changed)(void *ctx, wa_device *d);
} wa_client_callbacks;

typedef struct {
    const char *host;          // default "web.whatsapp.com"
    const char *path;          // default "/ws/chat"
    const char *origin;        // default "https://web.whatsapp.com"
    const char *ca_file;       // PEM bundle, or NULL to skip TLS verification (Noise still authenticates WA)
    int verify_tls;            // when ca_file is set
    
    // Fallback providers for domain fronting
    const wa_fallback_provider *fallback_providers;
    int fallback_count;
} wa_client_config;

wa_client *wa_client_new(wa_device *device, const wa_client_callbacks *cb, const wa_client_config *cfg);
void wa_client_free(wa_client *c);

// Opens the socket and runs the Noise handshake. Returns 0 (then poll/run) or -1 (on_disconnect already fired).
int wa_client_connect(wa_client *c);
// Pumps I/O for up to timeout_ms, dispatching stanzas and sending keepalives. Returns the current state, or
// WA_DISCONNECTED once the connection has ended.
wa_client_state wa_client_poll(wa_client *c, int timeout_ms);
// Convenience loop: connects (reconnecting on the expected post-pair drop) and polls until disconnected for real.
void wa_client_run(wa_client *c);
void wa_client_stop(wa_client *c);

wa_client_state wa_client_get_state(const wa_client *c);

// Sends a stanza (adds the frame + encryption). Returns 0 or -1.
int wa_client_send(wa_client *c, const wa_node *node);
// Sends an <iq> and returns its id (so the caller can match the result in on_node). buf must hold >= 24 chars.
int wa_client_send_iq(wa_client *c, wa_node *iq, char *id_out, size_t id_cap);
// A unique stanza id in WhatsApp's shape.
void wa_client_next_id(wa_client *c, char *out, size_t cap);

// An action posted from another thread, run on the client thread between polls (see wa_client_post).
typedef void (*wa_client_action)(wa_client *c, void *ctx);
// Enqueue an action to run on the client thread. Thread-safe. 0 or -1 (queue full).
int wa_client_post(wa_client *c, wa_client_action fn, void *ctx);
// Send an <iq> and block (pumping the socket) until the matching <iq result/error> arrives. CLIENT THREAD ONLY
// (call it from inside a posted action, never from another thread). Returns the result node (caller frees) or NULL.
wa_node *wa_client_request_sync(wa_client *c, wa_node *iq, int timeout_ms);
// The device identity (for the orchestration layer's key material).
wa_device *wa_client_device(wa_client *c);

#ifdef __cplusplus
}
#endif
#endif
