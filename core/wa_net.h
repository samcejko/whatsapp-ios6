// A WebSocket client over TLS (mbedTLS) for wss://web.whatsapp.com/ws/chat. The received binary messages are handed
// over as one byte stream (the Noise frames inside carry their own lengths). Single threaded: one owner thread
// calls every function.
#ifndef WA_NET_H
#define WA_NET_H

#include <stdint.h>
#include <stddef.h>
#include "wa_util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wa_ws wa_ws;

// Connects, runs the TLS and WebSocket handshakes. ca_file: a PEM bundle to verify the TLS certificate with (NULL:
// no TLS verification - the Noise handshake still authenticates WhatsApp's server key against WhatsApp's own root).
wa_ws *wa_ws_open(const char *host, int port, const char *path, const char *origin, const char *ca_file,
                  int timeout_ms, char *err, size_t errlen);
// Sends one binary message. 0 or -1.
int wa_ws_send(wa_ws *ws, const uint8_t *data, size_t len);
// Waits up to timeout_ms; appends received binary payload bytes to in. 1 = got data, 0 = timeout, -1 = closed/error.
int wa_ws_read(wa_ws *ws, wa_buf *in, int timeout_ms);
int wa_ws_fd(const wa_ws *ws);                 // the TCP socket (for poll/select or a VoIP stream wrapper)
const char *wa_ws_error(const wa_ws *ws);
void wa_ws_close(wa_ws *ws);

// One-time socket library setup (Winsock on Windows; nothing elsewhere)
void wa_net_init(void);

// A plain HTTPS GET (for the WhatsApp Web version lookup). Returns the body (malloc'd, NUL-terminated) or NULL.
char *wa_https_get(const char *host, const char *path, const char *ca_file, int timeout_ms, int *status);
// Binary-safe HTTPS GET (for media blobs, which contain NULs). Returns the body (malloc'd, *outlen bytes) or NULL.
uint8_t *wa_https_get_bin(const char *host, const char *path, const char *ca_file, int timeout_ms, int *status, size_t *outlen);

#ifdef __cplusplus
}
#endif
#endif
