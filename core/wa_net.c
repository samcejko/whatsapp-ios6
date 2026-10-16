#include "wa_net.h"
#include "wa_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define sock_close closesocket
static int sock_would_block(void) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAEINPROGRESS; }
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <strings.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close close
static int sock_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == EINPROGRESS; }
#endif

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

struct wa_ws {
    sock_t fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    int tls_ready;
    wa_buf raw;            // decrypted bytes not yet parsed into WebSocket frames
    wa_buf frag;           // payload of a fragmented message
    int frag_binary;
    int closed;
    char error[160];
};

void wa_net_init(void)
{
#ifdef _WIN32
    static int done;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = 1;
    }
#endif
}

static void set_nonblocking(sock_t fd)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    (void)one;
#endif
}

// 1 readable/writable, 0 timeout, -1 error
static int wait_fd(sock_t fd, int for_write, int timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select((int)fd + 1, for_write ? NULL : &set, for_write ? &set : NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    return r > 0 ? 1 : r;
}

// getaddrinfo() has no timeout and blocks until the OS resolver gives up (can be minutes on flaky DNS, e.g. a
// phone hotspot), which would hang the whole connect with no way for the client's connect-retry to kick in.
// Resolve on a detached worker and wait at most timeout_ms; if it overruns we abandon the worker (it frees
// itself when getaddrinfo finally returns) and report failure so the caller retries. Returns 0 + *out (caller
// frees) on success, -1 otherwise.
#ifndef _WIN32
#include <pthread.h>
struct wa_resolve_job {
    char host[256], port[8];
    struct addrinfo *res;
    int rc, done, abandoned;
    pthread_mutex_t m;
};
static void *wa_resolve_run(void *arg)
{
    struct wa_resolve_job *j = (struct wa_resolve_job *)arg;
    struct addrinfo hints, *r = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(j->host, j->port, &hints, &r);
    pthread_mutex_lock(&j->m);
    j->res = r;
    j->rc = rc;
    j->done = 1;
    int ab = j->abandoned;
    pthread_mutex_unlock(&j->m);
    if (ab) {                 // the caller already gave up: clean up after ourselves
        if (r) freeaddrinfo(r);
        pthread_mutex_destroy(&j->m);
        free(j);
    }
    return NULL;
}
static int resolve_timeout(const char *host, const char *port, int timeout_ms, struct addrinfo **out)
{
    struct wa_resolve_job *j = (struct wa_resolve_job *)calloc(1, sizeof *j);
    if (!j) return -1;
    snprintf(j->host, sizeof j->host, "%s", host);
    snprintf(j->port, sizeof j->port, "%s", port);
    pthread_mutex_init(&j->m, NULL);
    pthread_t th;
    if (pthread_create(&th, NULL, wa_resolve_run, j) != 0) {   // fall back to a blocking resolve
        pthread_mutex_destroy(&j->m);
        free(j);
        struct addrinfo hints;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        return (getaddrinfo(host, port, &hints, out) == 0 && *out) ? 0 : -1;
    }
    pthread_detach(th);
    uint64_t deadline = wa_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000;
    for (;;) {
        pthread_mutex_lock(&j->m);
        if (j->done) {
            int ok = (j->rc == 0 && j->res != NULL);
            *out = j->res;
            pthread_mutex_unlock(&j->m);
            pthread_mutex_destroy(&j->m);
            free(j);
            return ok ? 0 : -1;
        }
        if (wa_now_us() >= deadline) {
            j->abandoned = 1;     // worker frees itself once getaddrinfo returns
            pthread_mutex_unlock(&j->m);
            return -1;
        }
        pthread_mutex_unlock(&j->m);
        wa_sleep_ms(50);
    }
}
#endif

static sock_t tcp_connect(const char *host, int port, int timeout_ms, char *err, size_t errlen)
{
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo *res = NULL, *ai;
#ifdef _WIN32
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0 || !res) { snprintf(err, errlen, "cannot resolve %s", host); return SOCK_INVALID; }
#else
    if (resolve_timeout(host, portstr, timeout_ms, &res) != 0 || !res) {
        snprintf(err, errlen, "cannot resolve %s (dns timeout)", host);
        return SOCK_INVALID;
    }
#endif
    sock_t fd = SOCK_INVALID;
    // Prefer IPv4 and cap each attempt. On a cellular/hotspot link the IPv6 (AAAA) route often black-holes, and a
    // full 15 s stall on each dead address made the first connect — and the QR that waits on it — take forever.
    // Try A records first, then AAAA, giving each address at most ~5 s so a dead one falls through fast.
    int per = timeout_ms > 5000 ? 5000 : timeout_ms;
    for (int pass = 0; pass < 2 && fd == SOCK_INVALID; pass++) {
        int want = pass == 0 ? AF_INET : AF_INET6;
        for (ai = res; ai; ai = ai->ai_next) {
            if (ai->ai_family != want) continue;
            fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd == SOCK_INVALID) continue;
            set_nonblocking(fd);
            if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
            if (sock_would_block() && wait_fd(fd, 1, per) == 1) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
                if (soerr == 0) break;
            }
            sock_close(fd);
            fd = SOCK_INVALID;
        }
    }
    freeaddrinfo(res);
    if (fd == SOCK_INVALID) { snprintf(err, errlen, "cannot connect to %s:%d", host, port); return SOCK_INVALID; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&one, sizeof one);
    return fd;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    wa_ws *ws = (wa_ws *)ctx;
#ifdef _WIN32
    int n = send(ws->fd, (const char *)buf, (int)len, 0);
#elif defined(MSG_NOSIGNAL)
    ssize_t n = send(ws->fd, buf, len, MSG_NOSIGNAL);
#else
    ssize_t n = send(ws->fd, buf, len, 0);
#endif
    if (n < 0) return sock_would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    wa_ws *ws = (wa_ws *)ctx;
#ifdef _WIN32
    int n = recv(ws->fd, (char *)buf, (int)len, 0);
#else
    ssize_t n = recv(ws->fd, buf, len, 0);
#endif
    if (n < 0) return sock_would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (n == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    return (int)n;
}

static void tls_error(wa_ws *ws, const char *what, int rc)
{
    char e[100];
    mbedtls_strerror(rc, e, sizeof e);
    snprintf(ws->error, sizeof ws->error, "%s: %s (-0x%04x)", what, e, (unsigned)-rc);
}

// Writes everything, waiting for the socket when its buffer is full
static int tls_write_all(wa_ws *ws, const uint8_t *p, size_t len)
{
    while (len) {
        int n = mbedtls_ssl_write(&ws->ssl, p, len);
        if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
            if (wait_fd(ws->fd, n == MBEDTLS_ERR_SSL_WANT_WRITE, 10000) != 1) { snprintf(ws->error, sizeof ws->error, "write timeout"); return -1; }
            continue;
        }
        if (n <= 0) { tls_error(ws, "write", n); return -1; }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

// Reads what is available (waiting up to timeout_ms for the first bytes) into ws->raw. 1 data, 0 timeout, -1 error.
static int tls_read_some(wa_ws *ws, int timeout_ms)
{
    uint8_t tmp[16384];
    int got = 0;
    for (;;) {
        if (!got && mbedtls_ssl_get_bytes_avail(&ws->ssl) == 0) {
            int w = wait_fd(ws->fd, 0, timeout_ms);
            if (w == 0) return 0;
            if (w < 0) { snprintf(ws->error, sizeof ws->error, "select failed"); return -1; }
        }
        int n = mbedtls_ssl_read(&ws->ssl, tmp, sizeof tmp);
        if (n > 0) {
            if (wa_buf_append(&ws->raw, tmp, (size_t)n) != 0) return -1;
            got = 1;
            continue;
        }
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (got) return 1;
            timeout_ms = timeout_ms > 50 ? timeout_ms : 50;   // a partial record: give it a moment
            int w = wait_fd(ws->fd, 0, timeout_ms);
            if (w <= 0) return w;
            continue;
        }
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF) {
            snprintf(ws->error, sizeof ws->error, "connection closed by the server");
            return got ? 1 : -1;
        }
        tls_error(ws, "read", n);
        return got ? 1 : -1;
    }
}

static int tls_open(wa_ws *ws, const char *host, const char *ca_file, int timeout_ms)
{
    static const char pers[] = "wa6-tls";
    mbedtls_ssl_init(&ws->ssl);
    mbedtls_ssl_config_init(&ws->conf);
    mbedtls_entropy_init(&ws->entropy);
    mbedtls_ctr_drbg_init(&ws->drbg);
    mbedtls_x509_crt_init(&ws->ca);
    ws->tls_ready = 1;
    int rc = mbedtls_ctr_drbg_seed(&ws->drbg, mbedtls_entropy_func, &ws->entropy, (const unsigned char *)pers, sizeof pers - 1);
    if (rc != 0) { tls_error(ws, "drbg", rc); return -1; }
    rc = mbedtls_ssl_config_defaults(&ws->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { tls_error(ws, "config", rc); return -1; }
    mbedtls_ssl_conf_rng(&ws->conf, mbedtls_ctr_drbg_random, &ws->drbg);
    if (ca_file && mbedtls_x509_crt_parse_file(&ws->ca, ca_file) >= 0) {
        mbedtls_ssl_conf_ca_chain(&ws->conf, &ws->ca, NULL);
        mbedtls_ssl_conf_authmode(&ws->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&ws->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    rc = mbedtls_ssl_setup(&ws->ssl, &ws->conf);
    if (rc == 0) rc = mbedtls_ssl_set_hostname(&ws->ssl, host);
    if (rc != 0) { tls_error(ws, "setup", rc); return -1; }
    mbedtls_ssl_set_bio(&ws->ssl, ws, bio_send, bio_recv, NULL);
    uint64_t deadline = wa_now_us() + (uint64_t)timeout_ms * 1000;
    while ((rc = mbedtls_ssl_handshake(&ws->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) { tls_error(ws, "TLS handshake", rc); return -1; }
        uint64_t now = wa_now_us();
        if (now >= deadline) { snprintf(ws->error, sizeof ws->error, "TLS handshake timeout"); return -1; }
        wait_fd(ws->fd, rc == MBEDTLS_ERR_SSL_WANT_WRITE, (int)((deadline - now) / 1000) + 1);
    }
    return 0;
}

static wa_ws *ws_alloc(void)
{
    wa_ws *ws = (wa_ws *)calloc(1, sizeof *ws);
    if (!ws) return NULL;
    ws->fd = SOCK_INVALID;
    wa_buf_init(&ws->raw);
    wa_buf_init(&ws->frag);
    return ws;
}

static wa_ws *connect_tls(const char *host, int port, const char *ca_file, int timeout_ms, char *err, size_t errlen)
{
    wa_net_init();
    wa_ws *ws = ws_alloc();
    if (!ws) { snprintf(err, errlen, "out of memory"); return NULL; }
    ws->fd = tcp_connect(host, port, timeout_ms, err, errlen);
    if (ws->fd == SOCK_INVALID) { wa_ws_close(ws); return NULL; }
    if (tls_open(ws, host, ca_file, timeout_ms) != 0) {
        snprintf(err, errlen, "%s", ws->error);
        wa_ws_close(ws);
        return NULL;
    }
    return ws;
}

// Reads the HTTP status line and headers (up to the blank line) from ws->raw; returns the status or -1
static int read_http_head(wa_ws *ws, int timeout_ms, char *head, size_t headcap)
{
    uint64_t deadline = wa_now_us() + (uint64_t)timeout_ms * 1000;
    for (;;) {
        for (size_t i = 3; i < ws->raw.len; i++) {
            if (ws->raw.data[i - 3] == '\r' && ws->raw.data[i - 2] == '\n' && ws->raw.data[i - 1] == '\r' && ws->raw.data[i] == '\n') {
                size_t n = i + 1 < headcap ? i + 1 : headcap - 1;
                memcpy(head, ws->raw.data, n);
                head[n] = 0;
                wa_buf_consume(&ws->raw, i + 1);
                int status = 0;
                if (sscanf(head, "HTTP/%*s %d", &status) != 1) return -1;
                return status;
            }
        }
        uint64_t now = wa_now_us();
        if (now >= deadline) return -1;
        if (tls_read_some(ws, (int)((deadline - now) / 1000) + 1) < 0) return -1;
    }
}

wa_ws *wa_ws_open(const char *host, int port, const char *path, const char *origin, const char *ca_file,
                  int timeout_ms, char *err, size_t errlen)
{
    wa_ws *ws = connect_tls(host, port, ca_file, timeout_ms, err, errlen);
    if (!ws) return NULL;
    uint8_t nonce[16];
    char key[32];
    wa_random(nonce, sizeof nonce);
    wa_base64_encode(nonce, sizeof nonce, key, sizeof key);
    char req[1024];
    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "Origin: %s\r\n"
             "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36\r\n"
             "\r\n",
             path, host, key, origin);
    if (tls_write_all(ws, (const uint8_t *)req, strlen(req)) != 0) {
        snprintf(err, errlen, "%s", ws->error);
        wa_ws_close(ws);
        return NULL;
    }
    char head[2048];
    int status = read_http_head(ws, timeout_ms, head, sizeof head);
    if (status != 101) {
        snprintf(err, errlen, "WebSocket upgrade refused (HTTP %d)", status);
        wa_ws_close(ws);
        return NULL;
    }
    return ws;
}

static int send_frame(wa_ws *ws, int opcode, const uint8_t *data, size_t len)
{
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = (uint8_t)(0x80 | opcode);
    if (len < 126) hdr[h++] = (uint8_t)(0x80 | len);
    else if (len < 65536) { hdr[h++] = 0x80 | 126; wa_put_u16(hdr + h, (uint16_t)len); h += 2; }
    else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    uint8_t mask[4];
    wa_random(mask, 4);
    memcpy(hdr + h, mask, 4);
    h += 4;
    uint8_t *frame = (uint8_t *)malloc(h + len);
    if (!frame) return -1;
    memcpy(frame, hdr, h);
    for (size_t i = 0; i < len; i++) frame[h + i] = data[i] ^ mask[i & 3];
    int rc = tls_write_all(ws, frame, h + len);
    free(frame);
    return rc;
}

int wa_ws_send(wa_ws *ws, const uint8_t *data, size_t len)
{
    if (!ws || ws->closed) return -1;
    return send_frame(ws, 2, data, len);
}

// Parses complete frames out of ws->raw. Returns 1 when binary payload was appended to in, 0 if nothing, -1 closed.
static int parse_frames(wa_ws *ws, wa_buf *in)
{
    int got = 0;
    for (;;) {
        if (ws->raw.len < 2) return got;
        const uint8_t *p = ws->raw.data;
        int fin = p[0] & 0x80, opcode = p[0] & 0x0F, masked = p[1] & 0x80;
        uint64_t len = p[1] & 0x7F;
        size_t h = 2;
        if (len == 126) { if (ws->raw.len < 4) return got; len = wa_get_u16(p + 2); h = 4; }
        else if (len == 127) {
            if (ws->raw.len < 10) return got;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | p[2 + i];
            h = 10;
        }
        if (masked) h += 4;
        if (len > (64u << 20)) { snprintf(ws->error, sizeof ws->error, "oversized frame"); return -1; }
        if (ws->raw.len < h + len) return got;
        uint8_t *payload = ws->raw.data + h;
        if (masked) for (uint64_t i = 0; i < len; i++) payload[i] ^= p[h - 4 + (i & 3)];
        switch (opcode) {
        case 0:   // continuation
        case 1:
        case 2:
            if (opcode != 0) ws->frag_binary = opcode == 2;
            if (fin && ws->frag.len == 0) {
                if (ws->frag_binary) { wa_buf_append(in, payload, (size_t)len); got = 1; }
            } else {
                wa_buf_append(&ws->frag, payload, (size_t)len);
                if (fin) {
                    if (ws->frag_binary) { wa_buf_append(in, ws->frag.data, ws->frag.len); got = 1; }
                    wa_buf_clear(&ws->frag);
                }
            }
            break;
        case 8:
            snprintf(ws->error, sizeof ws->error, "server closed the WebSocket (code %d)", len >= 2 ? wa_get_u16(payload) : 0);
            send_frame(ws, 8, payload, len >= 2 ? 2 : 0);
            ws->closed = 1;
            wa_buf_consume(&ws->raw, h + (size_t)len);
            return got ? 1 : -1;
        case 9:
            send_frame(ws, 10, payload, (size_t)len);
            break;
        default:
            break;
        }
        wa_buf_consume(&ws->raw, h + (size_t)len);
    }
}

int wa_ws_read(wa_ws *ws, wa_buf *in, int timeout_ms)
{
    if (!ws || ws->closed) return -1;
    int got = parse_frames(ws, in);
    if (got) return got;
    uint64_t deadline = wa_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000;
    for (;;) {
        uint64_t now = wa_now_us();
        int left = now >= deadline ? 0 : (int)((deadline - now) / 1000);
        int r = tls_read_some(ws, left);
        if (r < 0) { ws->closed = 1; return -1; }
        if (r == 0) return 0;
        got = parse_frames(ws, in);
        if (got) return got;
        if (wa_now_us() >= deadline) return 0;
    }
}

int wa_ws_fd(const wa_ws *ws) { return ws ? (int)ws->fd : -1; }

const char *wa_ws_error(const wa_ws *ws) { return ws ? ws->error : "no connection"; }

void wa_ws_close(wa_ws *ws)
{
    if (!ws) return;
    if (ws->tls_ready) {
        if (!ws->closed && ws->fd != SOCK_INVALID) mbedtls_ssl_close_notify(&ws->ssl);
        mbedtls_ssl_free(&ws->ssl);
        mbedtls_ssl_config_free(&ws->conf);
        mbedtls_ctr_drbg_free(&ws->drbg);
        mbedtls_entropy_free(&ws->entropy);
        mbedtls_x509_crt_free(&ws->ca);
    }
    if (ws->fd != SOCK_INVALID) sock_close(ws->fd);
    wa_buf_free(&ws->raw);
    wa_buf_free(&ws->frag);
    free(ws);
}

// Shared HTTPS GET: fetches host/path into `out` (raw body bytes, binary-safe, no trailing NUL counted in out->len).
// Returns the HTTP status (> 0) or -1. The caller owns `out`.
static int https_fetch(const char *host, const char *path, const char *ca_file, int timeout_ms, wa_buf *out)
{
    char err[160];
    wa_ws *c = connect_tls(host, 443, ca_file, timeout_ms, err, sizeof err);
    if (!c) { wa_log("https %s: %s", host, err); return -1; }
    char req[1024];
    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
             "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
             path, host);
    char head[4096];
    int st = -1;
    if (tls_write_all(c, (const uint8_t *)req, strlen(req)) == 0) st = read_http_head(c, timeout_ms, head, sizeof head);
    if (st > 0) {
        long clen = -1;
        int chunked = 0;
        for (char *line = head; line && *line; ) {
            char *nl = strstr(line, "\r\n");
            if (nl) *nl = 0;
#ifdef _WIN32
            if (_strnicmp(line, "content-length:", 15) == 0) clen = atol(line + 15);
            if (_strnicmp(line, "transfer-encoding:", 18) == 0 && strstr(line, "chunked")) chunked = 1;
#else
            if (strncasecmp(line, "content-length:", 15) == 0) clen = atol(line + 15);
            if (strncasecmp(line, "transfer-encoding:", 18) == 0 && strstr(line, "chunked")) chunked = 1;
#endif
            line = nl ? nl + 2 : NULL;
        }
        uint64_t deadline = wa_now_us() + (uint64_t)timeout_ms * 1000;
        while ((clen < 0 || (long)c->raw.len < clen) && wa_now_us() < deadline) {
            int r = tls_read_some(c, 1000);
            if (r < 0) break;
            if (chunked && c->raw.len >= 5 && memcmp(c->raw.data + c->raw.len - 5, "0\r\n\r\n", 5) == 0) break;
        }
        wa_buf_append_byte(&c->raw, 0);   // a terminator for the string scans below (not part of the body)
        c->raw.len--;
        if (chunked) {
            size_t pos = 0;
            while (pos < c->raw.len) {
                long n = strtol((const char *)c->raw.data + pos, NULL, 16);
                const uint8_t *crlf = (const uint8_t *)strstr((const char *)c->raw.data + pos, "\r\n");
                if (n <= 0 || !crlf) break;
                pos = (size_t)(crlf - c->raw.data) + 2;
                if (pos + (size_t)n > c->raw.len) n = (long)(c->raw.len - pos);
                wa_buf_append(out, c->raw.data + pos, (size_t)n);
                pos += (size_t)n + 2;
            }
        } else {
            wa_buf_append(out, c->raw.data, c->raw.len);
        }
    }
    c->closed = 1;
    wa_ws_close(c);
    return st;
}

char *wa_https_get(const char *host, const char *path, const char *ca_file, int timeout_ms, int *status)
{
    if (status) *status = 0;
    wa_buf out;
    wa_buf_init(&out);
    int st = https_fetch(host, path, ca_file, timeout_ms, &out);
    if (status) *status = st;
    if (st <= 0) { wa_buf_free(&out); return NULL; }
    wa_buf_append_byte(&out, 0);   // NUL-terminate for the string API (not part of the body length)
    return (char *)wa_buf_detach(&out, NULL);
}

uint8_t *wa_https_get_bin(const char *host, const char *path, const char *ca_file, int timeout_ms, int *status, size_t *outlen)
{
    if (status) *status = 0;
    if (outlen) *outlen = 0;
    wa_buf out;
    wa_buf_init(&out);
    int st = https_fetch(host, path, ca_file, timeout_ms, &out);
    if (status) *status = st;
    if (st <= 0 || out.len == 0) { wa_buf_free(&out); return NULL; }
    size_t n = 0;
    uint8_t *body = wa_buf_detach(&out, &n);
    if (outlen) *outlen = n;
    return body;
}
