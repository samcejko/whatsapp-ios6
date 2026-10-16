#include "wa_store.h"
#include "wa_crypto.h"
#include "wa_proto.h"
#include "wa_node.h"   /* wa_jid */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The WhatsApp Web version string. Recent as of this build; refreshed at run time from the live meta when reachable.
static char g_version[32] = "2.3000.1047483476";

const char *wa_client_version(void) { return g_version; }

void wa_set_client_version(const char *v)
{
    if (v && *v && strlen(v) < sizeof g_version) strcpy(g_version, v);
}

static void gen_keypair(wa_keypair *k) { wa_curve_keypair(k->priv, k->pub); }

int wa_device_new(wa_device *d)
{
    memset(d, 0, sizeof *d);
    if (wa_random(&d->registration_id, 4) != 0) return -1;
    d->registration_id = (d->registration_id % 2147483646u) + 1;   // 1..=2147483647
    gen_keypair(&d->noise_key);
    gen_keypair(&d->identity_key);
    gen_keypair(&d->signed_pre_key);
    d->signed_pre_key_id = 1;

    // Signature over the 33-byte serialized signed-pre-key public (0x05 || pub), by the identity private key.
    uint8_t serialized[33];
    serialized[0] = 5;
    memcpy(serialized + 1, d->signed_pre_key.pub, 32);
    if (wa_curve_sign(d->signed_pre_key_sig, d->identity_key.priv, serialized, sizeof serialized) != 0) return -1;

    if (wa_random(d->adv_secret_key, 32) != 0) return -1;
    return 0;
}

// -------------------------------------------------------------------------------------------------- persistence

#define STORE_MAGIC 0x57413644u   // "WA6D"
#define STORE_VER 1

int wa_device_save(const wa_device *d, const char *path)
{
    wa_buf b;
    wa_buf_init(&b);
    uint8_t hdr[8];
    wa_put_u32(hdr, STORE_MAGIC);
    wa_put_u32(hdr + 4, STORE_VER);
    wa_buf_append(&b, hdr, 8);
    uint8_t rid[4];
    wa_put_u32(rid, d->registration_id);
    wa_buf_append(&b, rid, 4);
    wa_buf_append(&b, d->noise_key.priv, 32);
    wa_buf_append(&b, d->noise_key.pub, 32);
    wa_buf_append(&b, d->identity_key.priv, 32);
    wa_buf_append(&b, d->identity_key.pub, 32);
    wa_buf_append(&b, d->signed_pre_key.priv, 32);
    wa_buf_append(&b, d->signed_pre_key.pub, 32);
    uint8_t spk[4];
    wa_put_u32(spk, d->signed_pre_key_id);
    wa_buf_append(&b, spk, 4);
    wa_buf_append(&b, d->signed_pre_key_sig, 64);
    wa_buf_append(&b, d->adv_secret_key, 32);
    wa_buf_append_byte(&b, (uint8_t)d->registered);
    // length-prefixed strings and blobs
    const char *strs[3] = { d->pn, d->lid, d->push_name };
    for (int i = 0; i < 3; i++) {
        uint16_t n = (uint16_t)strlen(strs[i]);
        uint8_t ln[2];
        wa_put_u16(ln, n);
        wa_buf_append(&b, ln, 2);
        wa_buf_append(&b, strs[i], n);
    }
    uint8_t al[4];
    wa_put_u32(al, (uint32_t)d->account_len);
    wa_buf_append(&b, al, 4);
    wa_buf_append(&b, d->account, d->account_len);
    wa_buf_append_byte(&b, (uint8_t)d->have_server_static);
    wa_buf_append(&b, d->server_static, 32);

    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(b.data, 1, b.len, f) == b.len;
    if (f) fclose(f);
    wa_buf_free(&b);
    return ok ? 0 : -1;
}

typedef struct { const uint8_t *p, *end; int err; } rd;
static void rdcopy(rd *r, void *out, size_t n) { if ((size_t)(r->end - r->p) < n) { r->err = 1; return; } memcpy(out, r->p, n); r->p += n; }
static uint32_t rdu32(rd *r) { uint8_t b[4]; rdcopy(r, b, 4); return r->err ? 0 : wa_get_u32(b); }
static uint16_t rdu16(rd *r) { uint8_t b[2]; rdcopy(r, b, 2); return r->err ? 0 : wa_get_u16(b); }
static void rdstr(rd *r, char *out, size_t cap) { uint16_t n = rdu16(r); if (r->err || n >= cap) { r->err = 1; return; } rdcopy(r, out, n); out[n] = 0; }

int wa_device_load(wa_device *d, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1 << 20) { fclose(f); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    size_t got = buf ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (!buf || got != (size_t)sz) { free(buf); return -1; }

    memset(d, 0, sizeof *d);
    rd r = { buf, buf + sz, 0 };
    if (rdu32(&r) != STORE_MAGIC || rdu32(&r) != STORE_VER) { free(buf); return -1; }
    d->registration_id = rdu32(&r);
    rdcopy(&r, d->noise_key.priv, 32);
    rdcopy(&r, d->noise_key.pub, 32);
    rdcopy(&r, d->identity_key.priv, 32);
    rdcopy(&r, d->identity_key.pub, 32);
    rdcopy(&r, d->signed_pre_key.priv, 32);
    rdcopy(&r, d->signed_pre_key.pub, 32);
    d->signed_pre_key_id = rdu32(&r);
    rdcopy(&r, d->signed_pre_key_sig, 64);
    rdcopy(&r, d->adv_secret_key, 32);
    uint8_t reg = 0;
    rdcopy(&r, &reg, 1);
    d->registered = reg;
    rdstr(&r, d->pn, sizeof d->pn);
    rdstr(&r, d->lid, sizeof d->lid);
    rdstr(&r, d->push_name, sizeof d->push_name);
    uint32_t al = rdu32(&r);
    if (al <= sizeof d->account) { rdcopy(&r, d->account, al); d->account_len = al; } else r.err = 1;
    uint8_t hs = 0;
    rdcopy(&r, &hs, 1);
    d->have_server_static = hs;
    rdcopy(&r, d->server_static, 32);
    free(buf);
    return r.err ? -1 : 0;
}

// -------------------------------------------------------------------------------------------------- client payload

// Proto field numbers from the WhatsApp ClientPayload schema (see whatsapp-rust waproto, MIT).
enum { P_USERNAME = 1, P_PASSIVE = 3, P_USER_AGENT = 5, P_WEB_INFO = 6, P_CONNECT_TYPE = 12, P_CONNECT_REASON = 13,
       P_DEVICE = 18, P_DEVICE_PAIRING = 19, P_PULL = 33 };
enum { UA_PLATFORM = 1, UA_APP_VERSION = 2, UA_MCC = 3, UA_MNC = 4, UA_OS_VERSION = 5, UA_MANUFACTURER = 6,
       UA_DEVICE = 7, UA_OS_BUILD = 8, UA_RELEASE_CHANNEL = 10, UA_LOCALE_LANG = 11, UA_LOCALE_COUNTRY = 12 };
enum { AV_PRIMARY = 1, AV_SECONDARY = 2, AV_TERTIARY = 3 };
enum { DP_ERED = 1, DP_EKEYTYPE = 2, DP_EIDENT = 3, DP_ESKEY_ID = 4, DP_ESKEY_VAL = 5, DP_ESKEY_SIG = 6,
       DP_BUILD_HASH = 7, DP_DEVICE_PROPS = 8 };
#define PLATFORM_WEB 14
#define RELEASE_CHANNEL_RELEASE 0
#define CONNECT_TYPE_WIFI_UNKNOWN 1
#define CONNECT_REASON_USER_ACTIVATED 1

static void parse_version(uint32_t *a, uint32_t *b, uint32_t *c)
{
    *a = *b = *c = 0;
    sscanf(g_version, "%u.%u.%u", a, b, c);
}

static void build_app_version(wa_buf *av)
{
    uint32_t a, b, c;
    parse_version(&a, &b, &c);
    pb_put_varint(av, AV_PRIMARY, a);
    pb_put_varint(av, AV_SECONDARY, b);
    pb_put_varint(av, AV_TERTIARY, c);
}

static void build_user_agent(wa_buf *ua)
{
    wa_buf av;
    wa_buf_init(&av);
    build_app_version(&av);
    pb_put_varint(ua, UA_PLATFORM, PLATFORM_WEB);
    pb_put_message(ua, UA_APP_VERSION, &av);
    pb_put_string(ua, UA_MCC, "000");
    pb_put_string(ua, UA_MNC, "000");
    pb_put_string(ua, UA_OS_VERSION, "0.1.0");
    pb_put_string(ua, UA_MANUFACTURER, "");
    pb_put_string(ua, UA_DEVICE, "Desktop");
    pb_put_string(ua, UA_OS_BUILD, "0.1.0");
    pb_put_varint(ua, UA_RELEASE_CHANNEL, RELEASE_CHANNEL_RELEASE);
    pb_put_string(ua, UA_LOCALE_LANG, "en");
    pb_put_string(ua, UA_LOCALE_COUNTRY, "US");
    wa_buf_free(&av);
}

// DeviceProps: os="Wazzup", version 0.1.0, platformType=CHROME(1), requireFullSync=false
static void build_device_props(wa_buf *dp)
{
    wa_buf av;
    wa_buf_init(&av);
    pb_put_varint(&av, 1, 0);
    pb_put_varint(&av, 2, 1);
    pb_put_varint(&av, 3, 0);
    pb_put_string(dp, 1, "Wazzup");
    pb_put_message(dp, 2, &av);
    pb_put_varint(dp, 3, 1);        // platformType CHROME
    pb_put_bool(dp, 4, 0);          // requireFullSync
    wa_buf_free(&av);
}

int wa_build_client_payload(const wa_device *d, wa_buf *out)
{
    wa_buf ua;
    wa_buf_init(&ua);
    build_user_agent(&ua);
    pb_put_message(out, P_USER_AGENT, &ua);
    wa_buf_free(&ua);

    // WebInfo{ webSubPlatform = WEB_BROWSER }
    wa_buf web;
    wa_buf_init(&web);
    pb_put_varint(&web, 4, 0);
    pb_put_message(out, P_WEB_INFO, &web);
    wa_buf_free(&web);

    pb_put_varint(out, P_CONNECT_TYPE, CONNECT_TYPE_WIFI_UNKNOWN);
    pb_put_varint(out, P_CONNECT_REASON, CONNECT_REASON_USER_ACTIVATED);

    if (d->registered) {
        wa_jid me;
        if (wa_jid_parse(d->pn, &me) != 0) return -1;
        pb_put_varint(out, P_USERNAME, strtoull(me.user, NULL, 10));
        pb_put_varint(out, P_DEVICE, (uint64_t)me.device);
        pb_put_bool(out, P_PASSIVE, 0);
        pb_put_bool(out, P_PULL, 1);
    } else {
        wa_buf dp, reg;
        wa_buf_init(&dp);
        wa_buf_init(&reg);
        build_device_props(&dp);

        uint8_t rid[4];
        wa_put_u32(rid, d->registration_id);
        uint8_t keytype = 5;
        uint8_t skid[3] = { (uint8_t)(d->signed_pre_key_id >> 16), (uint8_t)(d->signed_pre_key_id >> 8), (uint8_t)d->signed_pre_key_id };
        char vbuf[32];
        snprintf(vbuf, sizeof vbuf, "%s", g_version);
        uint8_t build_hash[16];
        wa_md5(vbuf, strlen(vbuf), build_hash);

        pb_put_bytes(&reg, DP_ERED, rid, 4);
        pb_put_bytes(&reg, DP_EKEYTYPE, &keytype, 1);
        pb_put_bytes(&reg, DP_EIDENT, d->identity_key.pub, 32);
        pb_put_bytes(&reg, DP_ESKEY_ID, skid, 3);
        pb_put_bytes(&reg, DP_ESKEY_VAL, d->signed_pre_key.pub, 32);
        pb_put_bytes(&reg, DP_ESKEY_SIG, d->signed_pre_key_sig, 64);
        pb_put_bytes(&reg, DP_BUILD_HASH, build_hash, 16);
        pb_put_bytes(&reg, DP_DEVICE_PROPS, dp.data ? dp.data : (const uint8_t *)"", dp.len);

        pb_put_message(out, P_DEVICE_PAIRING, &reg);
        pb_put_bool(out, P_PASSIVE, 0);
        pb_put_bool(out, P_PULL, 0);
        wa_buf_free(&dp);
        wa_buf_free(&reg);
    }
    return 0;
}
