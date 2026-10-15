#include "wa_client.h"
#include "wa_net.h"
#include "wa_noise.h"
#include "wa_pair.h"
#include "wa_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <zlib.h>
#ifndef _WIN32
#include <pthread.h>
#endif

#define WA_MAX_PENDING 8
#define WA_MAX_ACTIONS 32

#define KEEPALIVE_MS 25000
#define QR_ROTATE_MS 20000

struct wa_client {
    wa_device *dev;
    wa_client_callbacks cb;
    wa_client_config cfg;
    wa_ws *ws;
    wa_noise_cipher enc, dec;
    wa_buf inbuf;             // accumulated decrypted WebSocket payload (framed noise messages)
    wa_client_state state;
    int stop;
    int reconnect_wanted;     // set after pair-success (expected 515)

    // pairing QR refs
    char refs[8][96];
    int nref, cur_ref;
    uint64_t next_qr_us;

    uint64_t last_send_us;
    uint32_t id_counter;
    char error[192];

    // IQ request/response correlation (used by wa_client_request_sync on the client thread)
    struct { char id[24]; wa_node *result; int done; int used; } pending[WA_MAX_PENDING];
    // actions posted from other threads, drained and run on the client thread
    struct { wa_client_action fn; void *ctx; } actions[WA_MAX_ACTIONS];
    int action_head, action_tail;
#ifndef _WIN32
    pthread_mutex_t lock;
    int lock_ready;
#endif
};

static void wc_lock(wa_client *c)
{
#ifndef _WIN32
    if (c->lock_ready) pthread_mutex_lock(&c->lock);
#else
    (void)c;
#endif
}

static void wc_unlock(wa_client *c)
{
#ifndef _WIN32
    if (c->lock_ready) pthread_mutex_unlock(&c->lock);
#else
    (void)c;
#endif
}

static void set_err(wa_client *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, sizeof c->error, fmt, ap);
    va_end(ap);
}

wa_client *wa_client_new(wa_device *device, const wa_client_callbacks *cb, const wa_client_config *cfg)
{
    wa_client *c = (wa_client *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->dev = device;
    if (cb) c->cb = *cb;
    if (cfg) c->cfg = *cfg;
    if (!c->cfg.host) c->cfg.host = "web.whatsapp.com";
    if (!c->cfg.path) c->cfg.path = "/ws/chat";
    if (!c->cfg.origin) c->cfg.origin = "https://web.whatsapp.com";
    wa_buf_init(&c->inbuf);
#ifndef _WIN32
    if (pthread_mutex_init(&c->lock, NULL) == 0) c->lock_ready = 1;
#endif
    return c;
}

void wa_client_free(wa_client *c)
{
    if (!c) return;
    if (c->ws) wa_ws_close(c->ws);
    wa_buf_free(&c->inbuf);
    for (int i = 0; i < WA_MAX_PENDING; i++) if (c->pending[i].result) wa_node_free(c->pending[i].result);
#ifndef _WIN32
    if (c->lock_ready) pthread_mutex_destroy(&c->lock);
#endif
    free(c);
}

wa_client_state wa_client_get_state(const wa_client *c) { return c ? c->state : WA_DISCONNECTED; }

wa_device *wa_client_device(wa_client *c) { return c ? c->dev : NULL; }

void wa_client_stop(wa_client *c) { if (c) c->stop = 1; }

void wa_client_next_id(wa_client *c, char *out, size_t cap)
{
    uint8_t r[8];
    wa_random(r, sizeof r);
    char hex[17];
    wa_hex_encode(r, 8, hex, 1);
    snprintf(out, cap, "%s", hex);
    (void)c;
}

// -------------------------------------------------------------------------------------------------- framing / send

// Sends one noise message as a WebSocket binary frame (length-prefixed). first adds the WA connection header.
static int ws_send_noise(wa_client *c, const uint8_t *data, size_t len, int first)
{
    wa_buf f;
    wa_buf_init(&f);
    int rc = wa_noise_frame(&f, data, len, first);
    if (rc == 0) rc = wa_ws_send(c->ws, f.data, f.len);
    wa_buf_free(&f);
    return rc;
}

int wa_client_send(wa_client *c, const wa_node *node)
{
    if (c->state < WA_HANDSHAKING || !c->ws) return -1;
    wa_buf plain, enc, frame;
    wa_buf_init(&plain);
    wa_buf_init(&enc);
    wa_buf_init(&frame);
    int rc = wa_node_encode(node, &plain);                 // leading format byte (0) + node
    if (rc == 0) rc = wa_noise_encrypt(&c->enc, plain.data, plain.len, &enc);
    if (rc == 0) rc = wa_noise_frame(&frame, enc.data, enc.len, 0);
    if (rc == 0) rc = wa_ws_send(c->ws, frame.data, frame.len);
    if (rc == 0) c->last_send_us = wa_now_us();
    wa_buf_free(&plain);
    wa_buf_free(&enc);
    wa_buf_free(&frame);
    return rc;
}

int wa_client_send_iq(wa_client *c, wa_node *iq, char *id_out, size_t id_cap)
{
    char id[24];
    const char *existing = wa_node_get(iq, "id");
    if (existing) snprintf(id, sizeof id, "%s", existing);
    else { wa_client_next_id(c, id, sizeof id); wa_node_attr(iq, "id", id); }
    if (id_out) snprintf(id_out, id_cap, "%s", id);
    return wa_client_send(c, iq);
}

// -------------------------------------------------------------------------------------------------- handshake

static int do_handshake(wa_client *c)
{
    c->state = WA_HANDSHAKING;
    wa_noise_hs hs;
    wa_buf hello;
    wa_buf_init(&hello);
    if (wa_noise_start(&hs, &hello) != 0) { set_err(c, "noise start failed"); wa_buf_free(&hello); return -1; }
    if (ws_send_noise(c, hello.data, hello.len, 1) != 0) { set_err(c, "cannot send ClientHello"); wa_buf_free(&hello); return -1; }
    wa_buf_free(&hello);

    // read the ServerHello frame
    wa_buf frame;
    wa_buf_init(&frame);
    uint64_t deadline = wa_now_us() + 20000000ull;
    int have = 0;
    while (!have && wa_now_us() < deadline) {
        int r = wa_ws_read(c->ws, &c->inbuf, 2000);
        if (r < 0) { set_err(c, "%s", wa_ws_error(c->ws)); wa_buf_free(&frame); return -1; }
        have = wa_noise_unframe(&c->inbuf, &frame) == 1;
    }
    if (!have) { set_err(c, "no ServerHello"); wa_buf_free(&frame); return -1; }

    wa_buf payload, finish;
    wa_buf_init(&payload);
    wa_buf_init(&finish);
    if (wa_build_client_payload(c->dev, &payload) != 0) { set_err(c, "cannot build client payload"); goto fail; }
    char err[160];
    if (wa_noise_finish(&hs, frame.data, frame.len, c->dev->noise_key.priv, c->dev->noise_key.pub,
                        payload.data, payload.len, &finish, err, sizeof err) != 0) {
        set_err(c, "%s", err);
        goto fail;
    }
    if (ws_send_noise(c, finish.data, finish.len, 0) != 0) { set_err(c, "cannot send ClientFinish"); goto fail; }
    wa_noise_split(&hs, &c->enc, &c->dec);
    memcpy(c->dev->server_static, hs.server_static, 32);
    c->dev->have_server_static = 1;
    wa_buf_free(&frame);
    wa_buf_free(&payload);
    wa_buf_free(&finish);
    return 0;
fail:
    wa_buf_free(&frame);
    wa_buf_free(&payload);
    wa_buf_free(&finish);
    return -1;
}

// -------------------------------------------------------------------------------------------------- node dispatch

static int inflate_payload(const uint8_t *in, size_t len, wa_buf *out)
{
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) return -1;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)len;
    uint8_t tmp[16384];
    int rc;
    do {
        z.next_out = tmp;
        z.avail_out = sizeof tmp;
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&z); return -1; }
        if (wa_buf_append(out, tmp, sizeof tmp - z.avail_out) != 0) { inflateEnd(&z); return -1; }
        if (out->len > (16u << 20)) { inflateEnd(&z); return -1; }
    } while (rc != Z_STREAM_END);
    inflateEnd(&z);
    return 0;
}

static void send_ack(wa_client *c, const wa_node *req)
{
    const char *id = wa_node_get(req, "id");
    const char *from = wa_node_get(req, "from");
    if (!id) return;
    wa_node *iq = wa_node_new("iq");
    wa_node_attr(iq, "to", from ? from : "@s.whatsapp.net");
    wa_node_attr(iq, "id", id);
    wa_node_attr(iq, "type", "result");
    wa_client_send(c, iq);
    wa_node_free(iq);
}

static void handle_pair_device(wa_client *c, const wa_node *iq, const wa_node *pd)
{
    send_ack(c, iq);
    c->nref = 0;
    for (int i = 0; i < pd->nchildren && c->nref < 8; i++) {
        wa_node *ref = pd->children[i];
        if (wa_node_tag_is(ref, "ref") && ref->has_content) {
            size_t n = ref->content_len < sizeof c->refs[0] ? ref->content_len : sizeof c->refs[0] - 1;
            memcpy(c->refs[c->nref], ref->content, n);
            c->refs[c->nref][n] = 0;
            c->nref++;
        }
    }
    c->cur_ref = 0;
    c->state = WA_PAIRING;
    c->next_qr_us = wa_now_us() + QR_ROTATE_MS * 1000ull;
    if (c->nref && c->cb.on_qr) {
        char *qr = wa_pair_qr_string(c->dev, c->refs[0], '1');
        if (qr) { c->cb.on_qr(c->cb.ctx, qr); free(qr); }
    }
}

static void handle_pair_success(wa_client *c, const wa_node *iq, const wa_node *ps)
{
    const char *req_id = wa_node_get(iq, "id");
    wa_node *di = wa_node_child(ps, "device-identity");
    if (!di || !di->has_content) {
        wa_node *e = wa_node_new("iq");
        wa_node_attr(e, "to", "@s.whatsapp.net");
        wa_node_attr(e, "type", "error");
        if (req_id) wa_node_attr(e, "id", req_id);
        wa_node *err = wa_node_add_new(e, "error");
        wa_node_attr(err, "code", "500");
        wa_node_attr(err, "text", "internal-error");
        wa_client_send(c, e);
        wa_node_free(e);
        return;
    }
    uint8_t out[2048];
    size_t out_len = 0;
    uint32_t key_index = 0;
    char err[160];
    if (wa_pair_complete(c->dev, di->content, di->content_len, out, sizeof out, &out_len, &key_index, err, sizeof err) != 0) {
        wa_log("pair: %s", err);
        wa_node *e = wa_node_new("iq");
        wa_node_attr(e, "to", "@s.whatsapp.net");
        wa_node_attr(e, "type", "error");
        if (req_id) wa_node_attr(e, "id", req_id);
        wa_node *en = wa_node_add_new(e, "error");
        wa_node_attr(en, "code", "401");
        wa_node_attr(en, "text", "not-authorized");
        wa_client_send(c, e);
        wa_node_free(e);
        return;
    }

    // capture our jid/lid and names
    wa_node *dev = wa_node_child(ps, "device");
    if (dev) {
        const char *jid = wa_node_get(dev, "jid");
        const char *lid = wa_node_get(dev, "lid");
        if (jid) snprintf(c->dev->pn, sizeof c->dev->pn, "%s", jid);
        if (lid) snprintf(c->dev->lid, sizeof c->dev->lid, "%s", lid);
    }
    wa_node *biz = wa_node_child(ps, "biz");
    if (biz && wa_node_get(biz, "name")) snprintf(c->dev->push_name, sizeof c->dev->push_name, "%s", wa_node_get(biz, "name"));
    c->dev->registered = 1;

    // reply: <iq type=result id=req><pair-device-sign><device-identity key-index=..>signed</></></iq>
    wa_node *reply = wa_node_new("iq");
    wa_node_attr(reply, "to", "@s.whatsapp.net");
    wa_node_attr(reply, "type", "result");
    if (req_id) wa_node_attr(reply, "id", req_id);
    wa_node *sign = wa_node_add_new(reply, "pair-device-sign");
    wa_node *outdi = wa_node_add_new(sign, "device-identity");
    wa_node_attrf(outdi, "key-index", "%u", key_index);
    wa_node_set_bytes(outdi, out, out_len);
    wa_client_send(c, reply);
    wa_node_free(reply);

    if (c->cb.on_device_changed) c->cb.on_device_changed(c->cb.ctx, c->dev);
    if (c->cb.on_paired) c->cb.on_paired(c->cb.ctx, c->dev->pn);
    c->reconnect_wanted = 1;   // the server sends a 515 next; reconnect with the login payload
}

// If node is an <iq> result/error whose id a request_sync is waiting on, stash a clone for it. Returns 1 if taken.
static int take_pending_iq(wa_client *c, const wa_node *node)
{
    if (!wa_node_tag_is(node, "iq")) return 0;
    const char *type = wa_node_get(node, "type");
    const char *id = wa_node_get(node, "id");
    if (!id || !type || (strcmp(type, "result") != 0 && strcmp(type, "error") != 0)) return 0;
    int taken = 0;
    wc_lock(c);
    for (int i = 0; i < WA_MAX_PENDING; i++) {
        if (c->pending[i].used && !c->pending[i].done && strcmp(c->pending[i].id, id) == 0) {
            c->pending[i].result = wa_node_clone(node);
            c->pending[i].done = 1;
            taken = 1;
            break;
        }
    }
    wc_unlock(c);
    return taken;
}

static void dispatch(wa_client *c, const wa_node *node)
{
    if (wa_node_tag_is(node, "iq")) {
        wa_node *pd = wa_node_child(node, "pair-device");
        wa_node *ps = wa_node_child(node, "pair-success");
        if (pd) { handle_pair_device(c, node, pd); return; }
        if (ps) { handle_pair_success(c, node, ps); return; }
        if (take_pending_iq(c, node)) return;
    } else if (wa_node_tag_is(node, "success")) {
        c->state = WA_CONNECTED;
        const char *pn = wa_node_get(node, "lid");
        (void)pn;
        if (c->cb.on_connected) c->cb.on_connected(c->cb.ctx);
        return;
    } else if (wa_node_tag_is(node, "failure")) {
        const char *reason = wa_node_get_or(node, "reason", "login failure");
        set_err(c, "server rejected login: %s", reason);
        c->stop = 1;
        return;
    } else if (wa_node_tag_is(node, "stream:error") || wa_node_tag_is(node, "xmlstreamend")) {
        const char *code = wa_node_get_or(node, "code", "");
        if (strcmp(code, "515") == 0 || c->reconnect_wanted) c->reconnect_wanted = 1;
        else set_err(c, "stream error %s", code);
        return;   // the read loop ends when the socket closes
    }
    if (c->cb.on_node) c->cb.on_node(c->cb.ctx, node);
}

// Takes complete noise frames out of inbuf, decrypts, unpacks and dispatches them.
static int process_frames(wa_client *c)
{
    wa_buf frame, plain;
    wa_buf_init(&frame);
    wa_buf_init(&plain);
    int rc = 0;
    while (wa_noise_unframe(&c->inbuf, &frame) == 1) {
        wa_buf_clear(&plain);
        if (wa_noise_decrypt(&c->dec, frame.data, frame.len, &plain) != 0) { set_err(c, "decrypt failed"); rc = -1; break; }
        if (plain.len == 0) continue;
        uint8_t flags = plain.data[0];
        wa_node *node;
        if (flags & 2) {
            wa_buf inflated;
            wa_buf_init(&inflated);
            if (inflate_payload(plain.data + 1, plain.len - 1, &inflated) != 0) { wa_buf_free(&inflated); set_err(c, "inflate failed"); rc = -1; break; }
            node = wa_node_decode(inflated.data, inflated.len);
            wa_buf_free(&inflated);
        } else {
            node = wa_node_decode(plain.data + 1, plain.len - 1);
        }
        if (!node) { set_err(c, "bad stanza"); continue; }
        dispatch(c, node);
        wa_node_free(node);
        if (c->stop || c->reconnect_wanted) break;
    }
    wa_buf_free(&frame);
    wa_buf_free(&plain);
    return rc;
}

// -------------------------------------------------------------------------------------------------- lifecycle

int wa_client_connect(wa_client *c)
{
    c->state = WA_CONNECTING;
    c->reconnect_wanted = 0;
    char err[192];
    c->ws = wa_ws_open(c->cfg.host, 443, c->cfg.path, c->cfg.origin,
                       c->cfg.verify_tls ? c->cfg.ca_file : NULL, 15000, err, sizeof err);
    if (!c->ws) {
        set_err(c, "%s", err);
        c->state = WA_DISCONNECTED;
        if (c->cb.on_disconnect) c->cb.on_disconnect(c->cb.ctx, c->error, 0);
        return -1;
    }
    if (do_handshake(c) != 0) {
        wa_ws_close(c->ws);
        c->ws = NULL;
        c->state = WA_DISCONNECTED;
        if (c->cb.on_disconnect) c->cb.on_disconnect(c->cb.ctx, c->error, 0);
        return -1;
    }
    c->last_send_us = wa_now_us();
    return 0;
}

static void send_keepalive(wa_client *c)
{
    wa_node *iq = wa_node_new("iq");
    char id[24];
    wa_client_next_id(c, id, sizeof id);
    wa_node_attr(iq, "id", id);
    wa_node_attr(iq, "to", "@s.whatsapp.net");
    wa_node_attr(iq, "type", "get");
    wa_node_attr(iq, "xmlns", "w:p");
    wa_node_add_new(iq, "ping");
    wa_client_send(c, iq);
    wa_node_free(iq);
}

// Drain actions posted from other threads and run them on the client thread.
static void drain_actions(wa_client *c)
{
    for (;;) {
        wa_client_action fn = NULL;
        void *ctx = NULL;
        wc_lock(c);
        if (c->action_head != c->action_tail) {
            fn = c->actions[c->action_head].fn;
            ctx = c->actions[c->action_head].ctx;
            c->action_head = (c->action_head + 1) % WA_MAX_ACTIONS;
        }
        wc_unlock(c);
        if (!fn) break;
        fn(c, ctx);
    }
}

int wa_client_post(wa_client *c, wa_client_action fn, void *ctx)
{
    int rc = -1;
    wc_lock(c);
    int next = (c->action_tail + 1) % WA_MAX_ACTIONS;
    if (next != c->action_head) { c->actions[c->action_tail].fn = fn; c->actions[c->action_tail].ctx = ctx; c->action_tail = next; rc = 0; }
    wc_unlock(c);
    return rc;
}

// Send an <iq> (assigns an id) and pump the socket until the matching result/error arrives. Client thread only.
wa_node *wa_client_request_sync(wa_client *c, wa_node *iq, int timeout_ms)
{
    if (c->state < WA_HANDSHAKING || !c->ws) return NULL;
    char id[24];
    const char *existing = wa_node_get(iq, "id");
    if (existing) snprintf(id, sizeof id, "%s", existing);
    else { wa_client_next_id(c, id, sizeof id); wa_node_attr(iq, "id", id); }

    int slot = -1;
    wc_lock(c);
    for (int i = 0; i < WA_MAX_PENDING; i++) if (!c->pending[i].used) { slot = i; break; }
    if (slot >= 0) { c->pending[slot].used = 1; c->pending[slot].done = 0; c->pending[slot].result = NULL; snprintf(c->pending[slot].id, sizeof c->pending[slot].id, "%s", id); }
    wc_unlock(c);
    if (slot < 0) return NULL;

    if (wa_client_send(c, iq) != 0) { wc_lock(c); c->pending[slot].used = 0; wc_unlock(c); return NULL; }

    uint64_t deadline = wa_now_us() + (uint64_t)timeout_ms * 1000;
    wa_node *result = NULL;
    while (wa_now_us() < deadline) {
        int done;
        wc_lock(c);
        done = c->pending[slot].done;
        if (done) { result = c->pending[slot].result; c->pending[slot].result = NULL; c->pending[slot].used = 0; }
        wc_unlock(c);
        if (done) break;
        int r = wa_ws_read(c->ws, &c->inbuf, 200);
        if (r < 0) break;
        if (r > 0 && process_frames(c) != 0) break;
    }
    wc_lock(c);
    c->pending[slot].used = 0;
    wc_unlock(c);
    return result;
}

wa_client_state wa_client_poll(wa_client *c, int timeout_ms)
{
    if (!c->ws || c->state == WA_DISCONNECTED) return WA_DISCONNECTED;
    drain_actions(c);
    if (c->reconnect_wanted) return WA_DISCONNECTED;
    int r = wa_ws_read(c->ws, &c->inbuf, timeout_ms);
    if (r < 0) {
        set_err(c, "%s", wa_ws_error(c->ws));
        c->state = WA_DISCONNECTED;
        if (c->cb.on_disconnect) c->cb.on_disconnect(c->cb.ctx, c->error, c->reconnect_wanted);
        return WA_DISCONNECTED;
    }
    if (r > 0 && process_frames(c) != 0) {
        c->state = WA_DISCONNECTED;
        if (c->cb.on_disconnect) c->cb.on_disconnect(c->cb.ctx, c->error, 0);
        return WA_DISCONNECTED;
    }
    uint64_t now = wa_now_us();
    // rotate the QR ref while pairing
    if (c->state == WA_PAIRING && c->nref > 1 && now >= c->next_qr_us) {
        c->cur_ref = (c->cur_ref + 1) % c->nref;
        c->next_qr_us = now + QR_ROTATE_MS * 1000ull;
        if (c->cb.on_qr) {
            char *qr = wa_pair_qr_string(c->dev, c->refs[c->cur_ref], '1');
            if (qr) { c->cb.on_qr(c->cb.ctx, qr); free(qr); }
        }
    }
    if (c->state == WA_CONNECTED && now - c->last_send_us >= KEEPALIVE_MS * 1000ull) send_keepalive(c);
    if (c->reconnect_wanted) return WA_DISCONNECTED;   // caller reconnects
    return c->state;
}

void wa_client_run(wa_client *c)
{
    for (;;) {
        if (wa_client_connect(c) != 0) return;
        while (!c->stop && !c->reconnect_wanted) {
            wa_client_state s = wa_client_poll(c, 1000);
            if (s == WA_DISCONNECTED) break;
        }
        if (c->ws) { wa_ws_close(c->ws); c->ws = NULL; }
        if (c->stop) { if (c->cb.on_disconnect) c->cb.on_disconnect(c->cb.ctx, c->error[0] ? c->error : "stopped", 0); return; }
        if (!c->reconnect_wanted) return;
        c->reconnect_wanted = 0;
        wa_sleep_ms(500);
    }
}
