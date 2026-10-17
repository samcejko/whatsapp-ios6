#include "wa_relay.h"
#include "wa_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void store_token(uint8_t (*arr)[256], size_t *lens, int *count, int cap, int id, const uint8_t *data, size_t len)
{
    if (id < 0 || id >= cap || len > 256) return;
    memcpy(arr[id], data, len);
    lens[id] = len;
    if (id + 1 > *count) *count = id + 1;
}

static void parse_indexed(const wa_node *relay, const char *tag, uint8_t (*arr)[256], size_t *lens, int *count, int cap)
{
    int auto_id = 0;
    for (int i = 0; i < relay->nchildren; i++) {
        wa_node *c = relay->children[i];
        if (!wa_node_tag_is(c, tag) || !c->has_content) continue;
        const char *ids = wa_node_get(c, "id");
        int id = ids ? atoi(ids) : auto_id;
        store_token(arr, lens, count, cap, id, c->content, c->content_len);
        auto_id = id + 1;
    }
}

// hbh_key content is 30 raw bytes, or base64 of 30 bytes.
static int decode_hbh(const uint8_t *data, size_t len, uint8_t out[30])
{
    if (len == 30) { memcpy(out, data, 30); return 1; }
    uint8_t dec[64];
    size_t n = wa_base64_decode((const char *)data, len, dec, sizeof dec);
    if (n == 30) { memcpy(out, dec, 30); return 1; }
    return 0;
}

int wa_relay_parse(const wa_node *relay, wa_relay_data *out)
{
    if (!wa_node_tag_is(relay, "relay")) return 0;
    memset(out, 0, sizeof *out);

    parse_indexed(relay, "token", out->tokens, out->token_len, &out->ntokens, WA_RELAY_MAX_TOKENS);
    parse_indexed(relay, "auth_token", out->auth_tokens, out->auth_token_len, &out->nauth_tokens, WA_RELAY_MAX_TOKENS);

    wa_node *hk = wa_node_child(relay, "hbh_key");
    if (!hk) hk = wa_node_child(relay, "key");
    if (hk && hk->has_content) {
        out->has_hbh_key = decode_hbh(hk->content, hk->content_len, out->hbh_key);
        if (hk->content_len <= sizeof out->relay_key) {
            memcpy(out->relay_key, hk->content, hk->content_len);
            out->relay_key_len = hk->content_len;
            out->has_relay_key = 1;
        }
    }
    wa_node *wl = wa_node_child(relay, "warp_mi_tag_len");
    if (wl && wl->has_content) {
        char buf[16];
        size_t n = wl->content_len < sizeof buf - 1 ? wl->content_len : sizeof buf - 1;
        memcpy(buf, wl->content, n);
        buf[n] = 0;
        long v = strtol(buf, NULL, 10);
        if (v > 0) out->warp_mi_tag_len = (uint32_t)v;
    }

    for (int i = 0; i < relay->nchildren && out->nendpoints < WA_RELAY_MAX_ENDPOINTS; i++) {
        wa_node *te2 = relay->children[i];
        if (!wa_node_tag_is(te2, "te2") || !te2->has_content) continue;
        wa_relay_endpoint *e = &out->endpoints[out->nendpoints];
        memset(e, 0, sizeof *e);
        e->relay_id = (int)wa_node_get_int(te2, "relay_id", 0);
        snprintf(e->relay_name, sizeof e->relay_name, "%s", wa_node_get_or(te2, "relay_name", ""));
        e->token_id = (int)wa_node_get_int(te2, "token_id", 0);
        e->auth_token_id = (int)wa_node_get_int(te2, "auth_token_id", 0);
        e->is_fna = strcmp(wa_node_get_or(te2, "is_fna", ""), "1") == 0;
        if (te2->content_len == 6) {
            snprintf(e->ipv4, sizeof e->ipv4, "%u.%u.%u.%u", te2->content[0], te2->content[1], te2->content[2], te2->content[3]);
            e->port = (uint16_t)((te2->content[4] << 8) | te2->content[5]);
            memcpy(e->te2_bytes, te2->content, 6);
            e->has_te2_bytes = 1;
        } else if (te2->content_len == 18) {
            char *p = e->ipv6;
            for (int j = 0; j < 16; j += 2) p += snprintf(p, sizeof e->ipv6 - (size_t)(p - e->ipv6), "%s%x", j ? ":" : "",
                                                           (te2->content[j] << 8) | te2->content[j + 1]);
            e->port = (uint16_t)((te2->content[16] << 8) | te2->content[17]);
        } else {
            continue;
        }
        out->nendpoints++;
    }
    return 1;
}

const wa_relay_endpoint *wa_relay_pick_endpoint(const wa_relay_data *d, int inbound)
{
    if (d->nendpoints == 0) return NULL;
    // Prefer webClientRelayPort (3480) for linked companion devices to guarantee bidirectional media
    for (int i = 0; i < d->nendpoints; i++) {
        if (d->endpoints[i].port == 3480) return &d->endpoints[i];
    }
    if (inbound) for (int i = 0; i < d->nendpoints; i++) if (d->endpoints[i].is_fna) return &d->endpoints[i];
    for (int i = 0; i < d->nendpoints; i++) if (!d->endpoints[i].is_fna) return &d->endpoints[i];
    return &d->endpoints[0];
}

int wa_relay_selftest(void)
{
    wa_node *relay = wa_node_new("relay");
    // <token id="0"> 8 bytes
    uint8_t tok[8] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 };
    wa_node *t = wa_node_add_new(relay, "token");
    wa_node_attr(t, "id", "0");
    wa_node_set_bytes(t, tok, 8);
    // <hbh_key> 30 bytes
    uint8_t hbh[30];
    for (int i = 0; i < 30; i++) hbh[i] = (uint8_t)(0x40 + i);
    wa_node_set_bytes(wa_node_add_new(relay, "hbh_key"), hbh, 30);
    // <warp_mi_tag_len>4</>
    wa_node_set_string(wa_node_add_new(relay, "warp_mi_tag_len"), "4");
    // two te2 endpoints: a normal one and an FNA one
    uint8_t a1[6] = { 157, 240, 226, 133, 0x0d, 0x96 };   // 157.240.226.133:3478
    wa_node *e1 = wa_node_add_new(relay, "te2");
    wa_node_attr(e1, "relay_name", "fra");
    wa_node_attr(e1, "token_id", "0");
    wa_node_set_bytes(e1, a1, 6);
    uint8_t a2[6] = { 31, 13, 64, 10, 0x0d, 0x96 };
    wa_node *e2 = wa_node_add_new(relay, "te2");
    wa_node_attr(e2, "relay_name", "lla");
    wa_node_attr(e2, "is_fna", "1");
    wa_node_set_bytes(e2, a2, 6);

    wa_relay_data d;
    int ok = wa_relay_parse(relay, &d) &&
             d.ntokens == 1 && d.token_len[0] == 8 && memcmp(d.tokens[0], tok, 8) == 0 &&
             d.has_hbh_key && memcmp(d.hbh_key, hbh, 30) == 0 &&
             d.warp_mi_tag_len == 4 &&
             d.nendpoints == 2 &&
             strcmp(d.endpoints[0].ipv4, "157.240.226.133") == 0 && d.endpoints[0].port == 3478 &&
             d.endpoints[0].has_te2_bytes && d.endpoints[1].is_fna;
    const wa_relay_endpoint *in = wa_relay_pick_endpoint(&d, 1);
    const wa_relay_endpoint *out = wa_relay_pick_endpoint(&d, 0);
    ok = ok && in && in->is_fna && out && !out->is_fna;
    wa_node_free(relay);
    if (!ok) { wa_log("relay selftest: parse/pick failed"); return -1; }
    return 0;
}
