// Relay media transport for calls: UDP -> DTLS -> SCTP -> the pre-negotiated WebRTC data channel to one WhatsApp
// relay endpoint, over which STUN (the WARP allocate) and WARP-framed SRTP media travel as binary messages
// (the WA Web flavour). Reuses the GFN6 plain-C WebRTC stack (gf_dtls + gf_sctp). Driven on its own thread.
// This is the live transport: it can only be exercised against a real relay, not in CI.
#ifndef WA_RTC_H
#define WA_RTC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wa_rtc wa_rtc;

typedef struct {
    void *ctx;
    void (*on_open)(void *ctx);                                   // the data channel is ready
    void (*on_packet)(void *ctx, const uint8_t *data, size_t len); // one binary message (STUN / RTP / RTCP)
    void (*on_closed)(void *ctx, const char *reason);
} wa_rtc_callbacks;

// Connect to the relay endpoint (dotted IPv4 + port). The pre-negotiated data channel label is WA Web's. The
// transport runs on its own thread; callbacks fire on that thread. Returns NULL on immediate failure.
wa_rtc *wa_rtc_connect(const char *relay_ip, uint16_t relay_port, const wa_rtc_callbacks *cb);
// Send one binary message (a WARP-framed SRTP packet or a STUN message) over the data channel. 0 or -1.
int wa_rtc_send(wa_rtc *r, const uint8_t *data, size_t len);
int wa_rtc_is_open(const wa_rtc *r);
void wa_rtc_close(wa_rtc *r);

#ifdef __cplusplus
}
#endif
#endif
