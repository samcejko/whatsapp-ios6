#include "wa_call.h"
#include "wa_crypto.h"
#include "wa_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Capability blobs observed in WhatsApp offers/accepts (ver=1).
static const uint8_t CAP_OFFER[7] = { 0x01, 0x05, 0xf7, 0x09, 0xe0, 0xbb, 0x13 };
static const uint8_t CAP_VIDEO_OFFER[7] = { 0x01, 0x05, 0xf7, 0x09, 0xe0, 0xfa, 0x13 };
static const uint8_t CAP_PREACCEPT[7] = { 0x01, 0x05, 0xf7, 0x09, 0xe0, 0xbb, 0x07 };

void wa_call_new_id(char *buf, size_t cap)
{
    uint8_t r[16];
    wa_random(r, sizeof r);
    char hex[33];
    wa_hex_encode(r, 16, hex, 1);
    snprintf(buf, cap, "%s", hex);
}

int wa_call_new_key(uint8_t call_key[32]) { return wa_random(call_key, 32); }

static wa_node *audio_opus(const char *rate)
{
    wa_node *n = wa_node_new("audio");
    wa_node_attr(n, "enc", "opus");
    wa_node_attr(n, "rate", rate);
    return n;
}

static wa_node *video_node(void)
{
    wa_node *n = wa_node_new("video");
    wa_node_attr(n, "enc", "vp8");       // WhatsApp advertises H.264 via the capability; the <video> marks a video call
    wa_node_attr(n, "orientation", "0");
    return n;
}

static wa_node *enc_node(const wa_call_devkey *dk)
{
    wa_node *n = wa_node_new("enc");
    wa_node_attr(n, "v", "2");
    wa_node_attr(n, "type", dk->enc_type);
    wa_node_attr(n, "count", "0");
    wa_node_set_bytes(n, dk->ciphertext, dk->ciphertext_len);
    return n;
}

// <call to=peer [id=wrapper]> <action call-id call-creator> children </action> </call>
static wa_node *call_wrap(const char *to, const char *wrapper_id, wa_node *action)
{
    wa_node *call = wa_node_new("call");
    wa_node_attr(call, "to", to);
    if (wrapper_id && wrapper_id[0]) {
        wa_node_attr(call, "id", wrapper_id);
    } else {
        char gen_id[33];
        wa_call_new_id(gen_id, sizeof gen_id);
        gen_id[20] = 0;
        wa_node_attr(call, "id", gen_id);
    }
    wa_node_add(call, action);
    return call;
}

static wa_node *action_node(const char *tag, const char *call_id, const char *call_creator)
{
    wa_node *a = wa_node_new(tag);
    wa_node_attr(a, "call-id", call_id);
    wa_node_attr(a, "call-creator", call_creator);
    return a;
}

static wa_node *destination_to(const char *const *jids, int n)
{
    wa_node *d = wa_node_new("destination");
    for (int i = 0; i < n; i++) {
        wa_node *to = wa_node_add_new(d, "to");
        wa_node_attr(to, "jid", jids[i]);
    }
    return d;
}

wa_node *wa_call_build_offer(const wa_call_offer *o)
{
    wa_node *offer = action_node("offer", o->call_id, o->call_creator);
    if (o->privacy_token) wa_node_set_bytes(wa_node_add_new(offer, "privacy"), o->privacy_token, o->privacy_token_len);
    wa_node_add(offer, audio_opus("8000"));
    wa_node_add(offer, audio_opus("16000"));
    if (o->video) wa_node_add(offer, video_node());
    wa_node_attr(wa_node_add_new(offer, "net"), "medium", "3");

    const uint8_t *cap = o->video ? CAP_VIDEO_OFFER : CAP_OFFER;
    wa_node *c = wa_node_add_new(offer, "capability");
    wa_node_attr(c, "ver", "1");
    wa_node_set_bytes(c, cap, 7);

    if (o->nkeys > 1) {
        wa_node *dest = wa_node_add_new(offer, "destination");
        for (int i = 0; i < o->nkeys; i++) {
            wa_node *to = wa_node_add_new(dest, "to");
            wa_node_attr(to, "jid", o->keys[i].device_jid);
            wa_node_add(to, enc_node(&o->keys[i]));
        }
    } else if (o->nkeys == 1) {
        wa_node_add(offer, enc_node(&o->keys[0]));
    }
    wa_node_attr(wa_node_add_new(offer, "encopt"), "keygen", "2");
    if (o->device_identity) wa_node_set_bytes(wa_node_add_new(offer, "device-identity"), o->device_identity, o->device_identity_len);
    return call_wrap(o->to, NULL, offer);
}

wa_node *wa_call_build_preaccept(const char *call_id, const char *to, const char *call_creator,
                                 const char *wrapper_id, int video)
{
    wa_node *pa = action_node("preaccept", call_id, call_creator);
    wa_node_add(pa, audio_opus("8000"));
    wa_node_add(pa, audio_opus("16000"));
    if (video) wa_node_add(pa, video_node());
    wa_node_attr(wa_node_add_new(pa, "encopt"), "keygen", "2");
    wa_node *c = wa_node_add_new(pa, "capability");
    wa_node_attr(c, "ver", "1");
    wa_node_set_bytes(c, video ? CAP_OFFER : CAP_PREACCEPT, 7);
    return call_wrap(to, wrapper_id, pa);
}

wa_node *wa_call_build_accept(const char *call_id, const char *to, const char *call_creator,
                              const char *const *audio_rates, int nrates,
                              const uint8_t *relay_te, size_t relay_te_len, int video)
{
    wa_node *ac = action_node("accept", call_id, call_creator);
    for (int i = 0; i < nrates; i++) wa_node_add(ac, audio_opus(audio_rates[i]));
    if (video) wa_node_add(ac, video_node());
    if (relay_te) {
        wa_node *te = wa_node_add_new(ac, "te");
        wa_node_attr(te, "priority", "2");
        wa_node_set_bytes(te, relay_te, relay_te_len);
    }
    wa_node_attr(wa_node_add_new(ac, "net"), "medium", "2");
    wa_node_attr(wa_node_add_new(ac, "encopt"), "keygen", "2");
    return call_wrap(to, NULL, ac);
}

wa_node *wa_call_build_transport(const char *call_id, const char *to, const char *call_creator,
                                 const uint8_t *relay_te, size_t relay_te_len, const char *msg_type)
{
    wa_node *t = wa_node_new("transport");
    wa_node_attr(t, "call-id", call_id);
    wa_node_attr(t, "call-creator", call_creator);
    if (msg_type) wa_node_attr(t, "transport-message-type", msg_type);
    if (relay_te) {
        wa_node *te = wa_node_add_new(t, "te");
        wa_node_attr(te, "priority", "1");
        wa_node_set_bytes(te, relay_te, relay_te_len);
    }
    wa_node *net = wa_node_add_new(t, "net");
    wa_node_attr(net, "medium", "2");
    if (!msg_type || strcmp(msg_type, "9") != 0) wa_node_attr(net, "protocol", "0");
    return call_wrap(to, NULL, t);
}

wa_node *wa_call_build_terminate(const char *call_id, const char *to, const char *call_creator, const char *reason)
{
    wa_node *t = wa_node_new("terminate");
    wa_node_attr(t, "call-id", call_id);
    wa_node_attr(t, "call-creator", call_creator);
    if (reason) wa_node_attr(t, "reason", reason);
    return call_wrap(to, NULL, t);
}

wa_node *wa_call_build_reject(const char *call_id, const char *to, const char *call_creator)
{
    wa_node *r = wa_node_new("reject");
    wa_node_attr(r, "call-id", call_id);
    wa_node_attr(r, "call-creator", call_creator);
    return call_wrap(to, NULL, r);
}

wa_node *wa_call_build_mute(const char *call_id, const char *to, const char *call_creator, int muted)
{
    wa_node *m = wa_node_new("mute_v2");
    wa_node_attr(m, "call-id", call_id);
    wa_node_attr(m, "call-creator", call_creator);
    wa_node_attr(m, "mute-state", muted ? "mute" : "unmute");
    return call_wrap(to, NULL, m);
}

wa_node *wa_call_build_relaylatency(const char *call_id, const char *to, const char *call_creator,
                                    uint32_t latency_ms, const char *relay_name, const uint8_t *addr, size_t addr_len)
{
    wa_node *rl = action_node("relaylatency", call_id, call_creator);
    wa_node *te = wa_node_add_new(rl, "te");
    char lat[16];
    snprintf(lat, sizeof lat, "%u", (unsigned)(0x02000000u + latency_ms));
    wa_node_attr(te, "latency", lat);
    wa_node_attr(te, "relay_name", relay_name);
    if (addr) wa_node_set_bytes(te, addr, addr_len);
    return call_wrap(to, NULL, rl);
}

wa_node *wa_call_build_heartbeat(const char *call_id, const char *call_creator, const char *wrapper_id)
{
    wa_node *hb = wa_node_new("heartbeat");
    wa_node_attr(hb, "call-id", call_id);
    wa_node_attr(hb, "call-creator", call_creator);
    char to[64];
    snprintf(to, sizeof to, "%s@call", call_id);
    return call_wrap(to, wrapper_id, hb);
}

// find the first <enc> anywhere under n
static const wa_node *find_enc(const wa_node *n)
{
    for (int i = 0; i < n->nchildren; i++) {
        if (wa_node_tag_is(n->children[i], "enc")) return n->children[i];
        const wa_node *e = find_enc(n->children[i]);
        if (e) return e;
    }
    return NULL;
}

int wa_call_parse_offer(const wa_node *call, wa_call_incoming *out)
{
    if (!wa_node_tag_is(call, "call")) return 0;
    wa_node *offer = wa_node_child(call, "offer");
    if (!offer) return 0;
    memset(out, 0, sizeof *out);
    out->from = wa_node_get(call, "from");
    out->call_id = wa_node_get(offer, "call-id");
    out->call_creator = wa_node_get(offer, "call-creator");
    out->video = wa_node_child(offer, "video") != NULL;
    out->enc = find_enc(offer);
    out->relay = wa_node_find(offer, "relay");
    return 1;
}

// -------------------------------------------------------------------------------------------------- self test

static wa_node *roundtrip(wa_node *n)
{
    wa_buf b;
    wa_buf_init(&b);
    wa_node *back = NULL;
    if (wa_node_encode(n, &b) == 0 && b.len > 1) back = wa_node_decode(b.data + 1, b.len - 1);
    wa_buf_free(&b);
    return back;
}

int wa_call_selftest(void)
{
    int ok = 1;
    char call_id[33];
    wa_call_new_id(call_id, sizeof call_id);
    if (strlen(call_id) != 32) { wa_log("call selftest: bad call id length"); return -1; }

    uint8_t ct[48];
    for (int i = 0; i < 48; i++) ct[i] = (uint8_t)(i * 7);
    wa_call_devkey dk = { "4915550001:0@s.whatsapp.net", ct, sizeof ct, "pkmsg" };
    uint8_t ident[80];
    for (int i = 0; i < 80; i++) ident[i] = (uint8_t)i;
    wa_call_offer o = { 0 };
    o.call_id = call_id;
    o.to = "4915550001@s.whatsapp.net";
    o.call_creator = "4915559999:3@s.whatsapp.net";
    o.keys = &dk;
    o.nkeys = 1;
    o.device_identity = ident;
    o.device_identity_len = sizeof ident;

    wa_node *offer = wa_call_build_offer(&o);
    wa_node *back = offer ? roundtrip(offer) : NULL;
    if (back) {
        wa_call_incoming in;
        ok = wa_call_parse_offer(back, &in) &&
             in.call_id && strcmp(in.call_id, call_id) == 0 &&
             in.enc && in.enc->content_len == sizeof ct && memcmp(in.enc->content, ct, sizeof ct) == 0 &&
             strcmp(wa_node_get_or(in.enc, "type", ""), "pkmsg") == 0;
        // child order inside <offer>: audio,audio,net,capability,enc,encopt,device-identity
        wa_node *ofn = wa_node_child(back, "offer");
        if (ok && ofn && ofn->nchildren >= 6) {
            const char *order[] = { "audio", "audio", "net", "capability", "enc", "encopt", "device-identity" };
            for (int i = 0; i < 7 && ok; i++) if (strcmp(ofn->children[i]->tag, order[i]) != 0) ok = 0;
        } else ok = 0;
    } else ok = 0;
    wa_node_free(offer);
    wa_node_free(back);
    if (!ok) { wa_log("call selftest: offer round trip/order failed"); return -1; }

    // accept + terminate round-trip structurally
    const char *rates[] = { "8000", "16000" };
    wa_node *acc = wa_call_build_accept(call_id, o.to, o.call_creator, rates, 2, NULL, 0, 0);
    wa_node *accb = acc ? roundtrip(acc) : NULL;
    ok = accb && wa_node_child(accb, "accept") && wa_node_child(wa_node_child(accb, "accept"), "encopt");
    wa_node_free(acc);
    wa_node_free(accb);
    if (!ok) { wa_log("call selftest: accept round trip failed"); return -1; }

    wa_node *term = wa_call_build_terminate(call_id, o.to, o.call_creator, "timeout");
    wa_node *termb = term ? roundtrip(term) : NULL;
    ok = termb && wa_node_child(termb, "terminate") &&
         strcmp(wa_node_get_or(wa_node_child(termb, "terminate"), "reason", ""), "timeout") == 0;
    wa_node_free(term);
    wa_node_free(termb);
    if (!ok) { wa_log("call selftest: terminate round trip failed"); return -1; }
    return 0;
}
