#include "wa_wstun.h"
#include "wa_crypto.h"

#include <string.h>
#include <stdio.h>

#define STUN_MAGIC 0x2112a442u
#define STUN_FP_XOR 0x5354554eu
#define STUN_XOR_PORT 0x2112u
static const uint8_t STUN_XOR_ADDR[4] = { 0x21, 0x12, 0xa4, 0x42 };

uint32_t wa_stun_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static size_t pad4(size_t n) { return (4 - (n % 4)) % 4; }

int wa_stun_put_attr(wa_buf *b, uint16_t type, const uint8_t *value, size_t vlen)
{
    uint8_t hdr[4];
    wa_put_u16(hdr, type);
    wa_put_u16(hdr + 2, (uint16_t)vlen);
    if (wa_buf_append(b, hdr, 4) != 0 || wa_buf_append(b, value, vlen) != 0) return -1;
    size_t p = pad4(vlen);
    for (size_t i = 0; i < p; i++) if (wa_buf_append_byte(b, 0) != 0) return -1;
    return 0;
}

static void pseudo_header(uint8_t h[20], uint16_t msg_type, uint16_t msg_len, const uint8_t tid[12])
{
    wa_put_u16(h, msg_type);
    wa_put_u16(h + 2, msg_len);
    wa_put_u32(h + 4, STUN_MAGIC);
    memcpy(h + 8, tid, 12);
}

int wa_stun_encode_request(wa_buf *out, uint16_t msg_type, const uint8_t tid[12], const uint8_t *attrs, size_t attrs_len,
                           const uint8_t *mi_key, size_t mi_key_len, int fingerprint)
{
    wa_buf body;
    wa_buf_init(&body);
    if (attrs_len && wa_buf_append(&body, attrs, attrs_len) != 0) { wa_buf_free(&body); return -1; }

    if (mi_key) {
        uint8_t hdr[20];
        pseudo_header(hdr, msg_type, (uint16_t)(body.len + 24), tid);   // + MI attr (4 + 20)
        uint8_t mi[20];
        wa_hmac_sha1_2(mi_key, mi_key_len, hdr, 20, body.data, body.len, mi);
        if (wa_stun_put_attr(&body, WA_STUN_ATTR_MESSAGE_INTEGRITY, mi, 20) != 0) { wa_buf_free(&body); return -1; }
    }
    if (fingerprint) {
        uint8_t hdr[20];
        pseudo_header(hdr, msg_type, (uint16_t)(body.len + 8), tid);    // + FINGERPRINT attr (4 + 4)
        wa_buf crc_in;
        wa_buf_init(&crc_in);
        wa_buf_append(&crc_in, hdr, 20);
        wa_buf_append(&crc_in, body.data, body.len);
        uint32_t fp = wa_stun_crc32(crc_in.data, crc_in.len) ^ STUN_FP_XOR;
        wa_buf_free(&crc_in);
        uint8_t fpb[4];
        wa_put_u32(fpb, fp);
        if (wa_stun_put_attr(&body, WA_STUN_ATTR_FINGERPRINT, fpb, 4) != 0) { wa_buf_free(&body); return -1; }
    }

    uint8_t hdr[20];
    wa_put_u16(hdr, msg_type);
    wa_put_u16(hdr + 2, (uint16_t)body.len);
    wa_put_u32(hdr + 4, STUN_MAGIC);
    memcpy(hdr + 8, tid, 12);
    int rc = wa_buf_append(out, hdr, 20);
    if (rc == 0) rc = wa_buf_append(out, body.data, body.len);
    wa_buf_free(&body);
    return rc;
}

void wa_stun_ping(uint8_t out[20], const uint8_t tid[12])
{
    wa_put_u16(out, WA_STUN_PING);
    wa_put_u16(out + 2, 0);
    wa_put_u32(out + 4, STUN_MAGIC);
    memcpy(out + 8, tid, 12);
}

int wa_stun_xor_endpoint(const char *ipv4, uint16_t port, uint8_t out[6])
{
    unsigned o[4];
    if (sscanf(ipv4, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]) != 4) return -1;
    uint16_t xport = port ^ STUN_XOR_PORT;
    wa_put_u16(out, xport);
    for (int i = 0; i < 4; i++) out[2 + i] = (uint8_t)o[i] ^ STUN_XOR_ADDR[i];
    return 0;
}

void wa_stun_native_sender_sub(uint32_t ssrc, uint8_t out[5])
{
    out[0] = 1;
    wa_put_u32(out + 1, ssrc);
}

int wa_stun_is_stun(const uint8_t *data, size_t len)
{
    return len >= 20 && (data[0] & 0xc0) == 0 && wa_get_u32(data + 4) == STUN_MAGIC;
}

int wa_stun_type(const uint8_t *data, size_t len, uint16_t *type)
{
    if (len < 2) return -1;
    *type = wa_get_u16(data);
    return 0;
}

const uint8_t *wa_stun_tid(const uint8_t *data, size_t len) { return len >= 20 ? data + 8 : NULL; }

int wa_stun_find_attr(const uint8_t *data, size_t len, uint16_t type, const uint8_t **val, size_t *vlen)
{
    if (len < 20) return 0;
    size_t pos = 20;
    while (pos + 4 <= len) {
        uint16_t t = wa_get_u16(data + pos);
        uint16_t l = wa_get_u16(data + pos + 2);
        if (pos + 4 + l > len) break;
        if (t == type) { *val = data + pos + 4; *vlen = l; return 1; }
        pos += 4 + l + pad4(l);
    }
    return 0;
}

int wa_stun_mapped_address(const uint8_t *data, size_t len, uint32_t *ip, uint16_t *port)
{
    const uint8_t *v;
    size_t vl;
    int xored = 0;
    if (wa_stun_find_attr(data, len, WA_STUN_ATTR_XOR_MAPPED_ADDRESS, &v, &vl)) xored = 1;
    else if (!wa_stun_find_attr(data, len, WA_STUN_ATTR_MAPPED_ADDRESS, &v, &vl)) return 0;
    if (vl < 8 || v[1] != 0x01) return 0;   // family 0x01 = IPv4
    uint16_t p = wa_get_u16(v + 2);
    uint32_t a = wa_get_u32(v + 4);
    if (xored) { p ^= (uint16_t)STUN_XOR_PORT; a ^= STUN_MAGIC; }
    *port = p;
    *ip = a;
    return 1;
}

// -------------------------------------------------------------------------------------------------- self test

static int eqhex(const char *label, const uint8_t *got, size_t n, const char *expect_hex)
{
    uint8_t exp[256];
    size_t en = strlen(expect_hex) / 2;
    if (en > sizeof exp || en != n) { wa_log("wstun selftest: %s length %u != %u", label, (unsigned)n, (unsigned)en); return -1; }
    wa_hex_decode(expect_hex, exp, sizeof exp);
    if (memcmp(got, exp, n) != 0) {
        char hex[513];
        wa_hex_encode(got, n < 256 ? n : 256, hex, 0);
        wa_log("wstun selftest: %s mismatch\n got  %s\n want %s", label, hex, expect_hex);
        return -1;
    }
    return 0;
}

int wa_wstun_selftest(void)
{
    int ok = 0;
    // crc32("abc") == 0x352441c2
    if (wa_stun_crc32((const uint8_t *)"abc", 3) != 0x352441c2u) { wa_log("wstun selftest: crc32(abc)"); ok |= 1; }

    static const uint8_t tid[12] = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab };
    static const uint8_t relay_token[8] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 };
    static const uint8_t mi_key[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

    wa_buf attr;
    wa_buf_init(&attr);
    wa_stun_put_attr(&attr, WA_STUN_ATTR_RELAY_TOKEN, relay_token, 8);
    ok |= eqhex("attr_token", attr.data, attr.len, "400000081020304050607080");

    wa_buf msg;
    wa_buf_init(&msg);
    wa_stun_encode_request(&msg, WA_STUN_ALLOCATE_REQUEST, tid, attr.data, attr.len, mi_key, 16, 0);
    ok |= eqhex("minimalMi", msg.data, msg.len,
                "000300242112a442a0a1a2a3a4a5a6a7a8a9aaab4000000810203040506070800008001450c96f7efef837bbde7e1f534dae139816d3e015");

    wa_buf msg2;
    wa_buf_init(&msg2);
    wa_stun_encode_request(&msg2, WA_STUN_ALLOCATE_REQUEST, tid, attr.data, attr.len, mi_key, 16, 1);
    ok |= eqhex("withFp", msg2.data, msg2.len,
                "0003002c2112a442a0a1a2a3a4a5a6a7a8a9aaab4000000810203040506070800008001450c96f7efef837bbde7e1f534dae139816d3e01580280004ade1f63c");

    uint8_t ping[20];
    wa_stun_ping(ping, tid);
    ok |= eqhex("ping", ping, 20, "080100002112a442a0a1a2a3a4a5a6a7a8a9aaab");

    uint8_t ep[6];
    wa_stun_xor_endpoint("157.240.226.133", 3478, ep);
    ok |= eqhex("xorEndpoint", ep, 6, "2c84bce246c7");

    uint8_t sub[5];
    wa_stun_native_sender_sub(305419896u, sub);
    ok |= eqhex("nativeSenderSub", sub, 5, "0112345678");

    // parse back the minimalMi message
    if (!wa_stun_is_stun(msg.data, msg.len)) { wa_log("wstun selftest: is_stun"); ok |= 1; }
    uint16_t t = 0;
    wa_stun_type(msg.data, msg.len, &t);
    if (t != WA_STUN_ALLOCATE_REQUEST) { wa_log("wstun selftest: type"); ok |= 1; }
    const uint8_t *tok;
    size_t tokn;
    if (!wa_stun_find_attr(msg.data, msg.len, WA_STUN_ATTR_RELAY_TOKEN, &tok, &tokn) || tokn != 8 || memcmp(tok, relay_token, 8) != 0) {
        wa_log("wstun selftest: find relay token");
        ok |= 1;
    }

    wa_buf_free(&attr);
    wa_buf_free(&msg);
    wa_buf_free(&msg2);
    return ok ? -1 : 0;
}
