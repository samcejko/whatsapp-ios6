#include "wa_user.h"
#include "wa_util.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SERVER_JID "@s.whatsapp.net"

wa_node *wa_user_build_devices_iq(const char *const *user_jids, int n, const char *sid)
{
    wa_node *iq = wa_node_new("iq");
    wa_node_attr(iq, "xmlns", "usync");
    wa_node_attr(iq, "type", "get");
    wa_node_attr(iq, "to", SERVER_JID);
    wa_node *usync = wa_node_add_new(iq, "usync");
    wa_node_attr(usync, "sid", sid);
    wa_node_attr(usync, "mode", "query");
    wa_node_attr(usync, "last", "true");
    wa_node_attr(usync, "index", "0");
    wa_node_attr(usync, "context", "message");
    wa_node *query = wa_node_add_new(usync, "query");
    wa_node_attr(wa_node_add_new(query, "devices"), "version", "2");
    wa_node *list = wa_node_add_new(usync, "list");
    for (int i = 0; i < n; i++) {
        wa_node *u = wa_node_add_new(list, "user");
        wa_node_attr(u, "jid", user_jids[i]);
    }
    return iq;
}

int wa_user_parse_devices(const wa_node *iq_result, wa_device_list *out)
{
    memset(out, 0, sizeof *out);
    const wa_node *usync = wa_node_find(iq_result, "usync");
    const wa_node *list = usync ? wa_node_child(usync, "list") : NULL;
    if (!list) return -1;
    for (int i = 0; i < list->nchildren; i++) {
        wa_node *user = list->children[i];
        if (!wa_node_tag_is(user, "user")) continue;
        const char *ujid = wa_node_get(user, "jid");
        if (!ujid) continue;
        wa_jid uj;
        if (wa_jid_parse(ujid, &uj) != 0) continue;
        const wa_node *devlist = wa_node_find(user, "device-list");
        if (!devlist) continue;
        for (int k = 0; k < devlist->nchildren && out->ndevices < WA_MAX_DEVICES; k++) {
            wa_node *dev = devlist->children[k];
            if (!wa_node_tag_is(dev, "device")) continue;
            int id = (int)wa_node_get_int(dev, "id", -1);
            if (id < 0) continue;
            wa_jid_format(out->jids[out->ndevices], sizeof out->jids[0], uj.user, id, uj.server);
            out->ndevices++;
        }
    }
    return out->ndevices > 0 ? 0 : -1;
}

wa_node *wa_user_build_prekey_iq(const char *const *device_jids, int n)
{
    wa_node *iq = wa_node_new("iq");
    wa_node_attr(iq, "xmlns", "encrypt");
    wa_node_attr(iq, "type", "get");
    wa_node_attr(iq, "to", SERVER_JID);
    wa_node *key = wa_node_add_new(iq, "key");
    for (int i = 0; i < n; i++) {
        wa_node *u = wa_node_add_new(key, "user");
        wa_node_attr(u, "jid", device_jids[i]);
        wa_node_attr(u, "reason", "identity");
    }
    return iq;
}

// A <value>/<identity> may be 32 raw bytes or 33 with a leading 0x05 type byte; return the 32-byte key.
static int key32(const wa_node *n, uint8_t out[32])
{
    if (!n || !n->has_content) return -1;
    if (n->content_len == 32) { memcpy(out, n->content, 32); return 0; }
    if (n->content_len == 33 && n->content[0] == 5) { memcpy(out, n->content + 1, 32); return 0; }
    return -1;
}

static uint32_t id_be(const wa_node *n)
{
    if (!n || !n->has_content) return 0;
    uint32_t v = 0;
    for (size_t i = 0; i < n->content_len && i < 4; i++) v = (v << 8) | n->content[i];
    return v;
}

int wa_user_parse_bundle(const wa_node *user_node, wa_sig_bundle *out)
{
    if (!wa_node_tag_is(user_node, "user")) return -1;
    if (wa_node_child(user_node, "error")) return -1;
    const wa_node *keys = wa_node_child(user_node, "keys");
    if (!keys) keys = user_node;

    memset(out, 0, sizeof *out);
    const wa_node *reg = wa_node_child(user_node, "registration");
    out->registration_id = id_be(reg);

    const wa_node *identity = wa_node_child(keys, "identity");
    if (key32(identity, out->identity_pub) != 0) return -1;

    const wa_node *skey = wa_node_child(keys, "skey");
    if (!skey) return -1;
    out->signed_prekey_id = id_be(wa_node_child(skey, "id"));
    if (key32(wa_node_child(skey, "value"), out->signed_prekey_pub) != 0) return -1;
    const wa_node *sig = wa_node_child(skey, "signature");
    if (!sig || sig->content_len != 64) return -1;
    memcpy(out->signed_prekey_sig, sig->content, 64);

    const wa_node *pk = wa_node_child(keys, "key");
    if (pk && key32(wa_node_child(pk, "value"), out->one_time_pub) == 0) {
        out->has_one_time = 1;
        out->prekey_id = id_be(wa_node_child(pk, "id"));
    }
    return 0;
}

int wa_user_selftest(void)
{
    // Build a usync response and parse the device list.
    wa_node *iq = wa_node_new("iq");
    wa_node *usync = wa_node_add_new(iq, "usync");
    wa_node *list = wa_node_add_new(usync, "list");
    wa_node *user = wa_node_add_new(list, "user");
    wa_node_attr(user, "jid", "420777123456@s.whatsapp.net");
    wa_node *devices = wa_node_add_new(user, "devices");
    wa_node *dl = wa_node_add_new(devices, "device-list");
    for (int i = 0; i < 3; i++) { wa_node *d = wa_node_add_new(dl, "device"); wa_node_attrf(d, "id", "%d", i); }
    wa_device_list dlist;
    int ok = wa_user_parse_devices(iq, &dlist) == 0 && dlist.ndevices == 3 &&
             strcmp(dlist.jids[0], "420777123456@s.whatsapp.net") == 0 &&
             strcmp(dlist.jids[1], "420777123456:1@s.whatsapp.net") == 0;
    wa_node_free(iq);
    if (!ok) { wa_log("user selftest: device list parse failed"); return -1; }

    // Build a prekey response and parse a bundle (ids 3-byte BE, keys 32 raw).
    uint8_t ident[32], spk[32], otp[32], sig[64];
    for (int i = 0; i < 32; i++) { ident[i] = (uint8_t)i; spk[i] = (uint8_t)(i + 1); otp[i] = (uint8_t)(i + 2); }
    for (int i = 0; i < 64; i++) sig[i] = (uint8_t)(i + 3);
    wa_node *u2 = wa_node_new("user");
    wa_node_attr(u2, "jid", "420777123456:1@s.whatsapp.net");
    uint8_t reg[4] = { 0, 0, 0x12, 0x34 };
    wa_node_set_bytes(wa_node_add_new(u2, "registration"), reg, 4);
    wa_node_set_bytes(wa_node_add_new(u2, "identity"), ident, 32);
    wa_node *sk = wa_node_add_new(u2, "skey");
    uint8_t skid[3] = { 0, 0, 7 };
    wa_node_set_bytes(wa_node_add_new(sk, "id"), skid, 3);
    wa_node_set_bytes(wa_node_add_new(sk, "value"), spk, 32);
    wa_node_set_bytes(wa_node_add_new(sk, "signature"), sig, 64);
    wa_node *pk = wa_node_add_new(u2, "key");
    uint8_t pkid[3] = { 0, 0, 42 };
    wa_node_set_bytes(wa_node_add_new(pk, "id"), pkid, 3);
    wa_node_set_bytes(wa_node_add_new(pk, "value"), otp, 32);
    wa_sig_bundle b;
    ok = wa_user_parse_bundle(u2, &b) == 0 && b.registration_id == 0x1234 && b.signed_prekey_id == 7 &&
         b.prekey_id == 42 && b.has_one_time && memcmp(b.identity_pub, ident, 32) == 0 &&
         memcmp(b.signed_prekey_pub, spk, 32) == 0 && memcmp(b.one_time_pub, otp, 32) == 0 &&
         memcmp(b.signed_prekey_sig, sig, 64) == 0;
    wa_node_free(u2);
    if (!ok) { wa_log("user selftest: bundle parse failed"); return -1; }
    return 0;
}
