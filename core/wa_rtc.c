#include "wa_rtc.h"
#include "wa_util.h"
#include "rtc/gf_dtls.h"
#include "rtc/gf_sctp.h"

#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET rtc_sock;
#define RTC_BAD INVALID_SOCKET
#define rtc_close closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
typedef int rtc_sock;
#define RTC_BAD (-1)
#define rtc_close close
#endif

// WA Web's pre-negotiated data channel: id 0, label "pre-negotiated".
#define WA_DC_STREAM 0
#define WA_DC_LABEL "pre-negotiated"

struct wa_rtc {
    rtc_sock fd;
    gf_dtls *dtls;
    gf_sctp *sctp;
    wa_rtc_callbacks cb;
    int running, open;
#ifndef _WIN32
    pthread_t thread;
#endif
    volatile int stop;
};

// gf_dtls io.send -> UDP socket (connected)
static int dtls_send(void *ctx, const uint8_t *data, size_t len)
{
    wa_rtc *r = (wa_rtc *)ctx;
#ifdef _WIN32
    return send(r->fd, (const char *)data, (int)len, 0) < 0 ? -1 : 0;
#else
    return send(r->fd, data, len, 0) < 0 ? -1 : 0;
#endif
}

// gf_sctp send -> DTLS application data
static int sctp_send(void *ctx, const uint8_t *pkt, size_t len)
{
    wa_rtc *r = (wa_rtc *)ctx;
    return gf_dtls_write(r->dtls, pkt, len);
}

static void sctp_connected(void *ctx)
{
    wa_rtc *r = (wa_rtc *)ctx;
    gf_sctp_open_channel(r->sctp, WA_DC_STREAM, WA_DC_LABEL, "", 1, 0, 0);
}

static void sctp_channel_ack(void *ctx, uint16_t stream)
{
    wa_rtc *r = (wa_rtc *)ctx;
    if (stream == WA_DC_STREAM && !r->open) {
        r->open = 1;
        if (r->cb.on_open) r->cb.on_open(r->cb.ctx);
    }
}

static void sctp_channel_open(void *ctx, uint16_t stream, const char *label, const char *protocol)
{
    (void)label;
    (void)protocol;
    wa_rtc *r = (wa_rtc *)ctx;
    if (stream == WA_DC_STREAM && !r->open) {
        r->open = 1;
        if (r->cb.on_open) r->cb.on_open(r->cb.ctx);
    }
}

static void sctp_message(void *ctx, uint16_t stream, uint32_t ppid, const uint8_t *data, size_t len)
{
    (void)stream;
    (void)ppid;
    wa_rtc *r = (wa_rtc *)ctx;
    if (r->cb.on_packet) r->cb.on_packet(r->cb.ctx, data, len);
}

static void sctp_closed(void *ctx, const char *reason)
{
    wa_rtc *r = (wa_rtc *)ctx;
    if (r->cb.on_closed) r->cb.on_closed(r->cb.ctx, reason);
}

static void run_loop(wa_rtc *r)
{
    gf_dtls_io io = { r, dtls_send };
    if (gf_dtls_start(r->dtls, &io, NULL, wa_now_us()) != 0) {
        if (r->cb.on_closed) r->cb.on_closed(r->cb.ctx, "dtls start failed");
        return;
    }
    uint8_t buf[2048];
    int sctp_started = 0;
    while (!r->stop) {
        uint64_t now = wa_now_us();
        // pump the socket (non-blocking read)
#ifdef _WIN32
        int n = recv(r->fd, (char *)buf, (int)sizeof buf, 0);
#else
        ssize_t n = recv(r->fd, buf, sizeof buf, 0);
#endif
        if (n > 0) {
            int hs = gf_dtls_input(r->dtls, buf, (size_t)n, now);
            if (hs < 0) { if (r->cb.on_closed) r->cb.on_closed(r->cb.ctx, "dtls error"); break; }
            // drain DTLS application data into SCTP
            int rd;
            while ((rd = gf_dtls_read(r->dtls, buf, sizeof buf)) > 0) gf_sctp_input(r->sctp, buf, (size_t)rd, now);
        }
        if (!sctp_started && gf_dtls_is_connected(r->dtls)) {
            gf_sctp_connect(r->sctp, now);
            sctp_started = 1;
        }
        gf_dtls_tick(r->dtls, now);
        if (sctp_started) gf_sctp_tick(r->sctp, now);
        wa_sleep_ms(5);
    }
}

#ifndef _WIN32
static void *thread_main(void *arg)
{
    wa_rtc *r = (wa_rtc *)arg;
    run_loop(r);
    return NULL;
}
#endif

wa_rtc *wa_rtc_connect(const char *relay_ip, uint16_t relay_port, const wa_rtc_callbacks *cb)
{
    wa_rtc *r = (wa_rtc *)calloc(1, sizeof *r);
    if (!r) return NULL;
    if (cb) r->cb = *cb;
    r->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (r->fd == RTC_BAD) { free(r); return NULL; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(relay_port);
    sa.sin_addr.s_addr = inet_addr(relay_ip);
    if (connect(r->fd, (struct sockaddr *)&sa, sizeof sa) != 0) { rtc_close(r->fd); free(r); return NULL; }
#ifndef _WIN32
    fcntl(r->fd, F_SETFL, fcntl(r->fd, F_GETFL, 0) | O_NONBLOCK);
#else
    u_long on = 1;
    ioctlsocket(r->fd, FIONBIO, &on);
#endif
    r->dtls = gf_dtls_create();
    gf_sctp_callbacks sc = { r, sctp_send, sctp_connected, sctp_channel_open, sctp_channel_ack, sctp_message, sctp_closed };
    r->sctp = gf_sctp_create(5000, 5000, &sc, wa_now_us());
    if (!r->dtls || !r->sctp) { wa_rtc_close(r); return NULL; }
    r->running = 1;
#ifndef _WIN32
    if (pthread_create(&r->thread, NULL, thread_main, r) != 0) { wa_rtc_close(r); return NULL; }
#else
    run_loop(r);   // the desktop prototype on Windows runs it inline; iOS/Linux use the thread
#endif
    return r;
}

int wa_rtc_send(wa_rtc *r, const uint8_t *data, size_t len)
{
    if (!r || !r->open) return -1;
    return gf_sctp_send(r->sctp, WA_DC_STREAM, GF_SCTP_PPID_BINARY, data, len, 1);
}

int wa_rtc_is_open(const wa_rtc *r) { return r && r->open; }

void wa_rtc_close(wa_rtc *r)
{
    if (!r) return;
    r->stop = 1;
#ifndef _WIN32
    if (r->running) pthread_join(r->thread, NULL);
#endif
    if (r->sctp) gf_sctp_destroy(r->sctp);
    if (r->dtls) gf_dtls_destroy(r->dtls);
    if (r->fd != RTC_BAD) rtc_close(r->fd);
    free(r);
}
