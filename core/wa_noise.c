#include "wa_noise.h"
#include "wa_crypto.h"
#include "wa_proto.h"

#include <string.h>
#include <stdio.h>

const uint8_t WA_CONN_HEADER[4] = { 'W', 'A', 6, 3 };

static const char NOISE_XX[32] = "Noise_XX_25519_AESGCM_SHA256\0\0\0";   // 28 chars + 4 NULs (the 4th is the literal's)

// WhatsApp's certificate authority (Curve25519, Signal-style signatures) for the server's Noise certificates
static const uint8_t WA_CERT_PUB_KEY[32] = {
    0x14, 0x23, 0x75, 0x57, 0x4d, 0x0a, 0x58, 0x71, 0x66, 0xaa, 0xe7, 0x1e, 0xbe, 0x51, 0x64, 0x37,
    0xc4, 0xa2, 0x8b, 0x73, 0xe3, 0x69, 0x5c, 0x6c, 0xe1, 0xf7, 0xf9, 0x54, 0x5d, 0xa8, 0xee, 0x6b };

static void iv_for(uint32_t counter, uint8_t iv[12])
{
    memset(iv, 0, 12);
    wa_put_u32(iv + 8, counter);
}

static void mix_hash(wa_noise_hs *hs, const uint8_t *data, size_t len)
{
    wa_sha256_2(hs->h, 32, data, len, hs->h);
}

static int mix_key(wa_noise_hs *hs, const uint8_t *ikm, size_t len)
{
    uint8_t out[64];
    if (wa_hkdf_sha256(hs->ck, 32, ikm, len, NULL, 0, out, 64) != 0) return -1;
    memcpy(hs->ck, out, 32);
    memcpy(hs->k, out + 32, 32);
    hs->n = 0;
    hs->has_key = 1;
    wa_wipe(out, sizeof out);
    return 0;
}

static int mix_dh(wa_noise_hs *hs, const uint8_t priv[32], const uint8_t pub[32])
{
    uint8_t shared[32];
    if (wa_curve_dh(shared, priv, pub) != 0) return -1;
    int rc = mix_key(hs, shared, 32);
    wa_wipe(shared, sizeof shared);
    return rc;
}

static int encrypt_and_hash(wa_noise_hs *hs, const uint8_t *pt, size_t len, wa_buf *out)
{
    uint8_t iv[12];
    iv_for(hs->n++, iv);
    size_t start = out->len;
    if (wa_buf_reserve(out, len + 16) != 0) return -1;
    if (wa_aes_gcm_encrypt(hs->k, 32, iv, 12, hs->h, 32, pt, len, out->data + start) != 0) return -1;
    out->len += len + 16;
    mix_hash(hs, out->data + start, len + 16);
    return 0;
}

static int decrypt_and_hash(wa_noise_hs *hs, const uint8_t *ct, size_t len, wa_buf *out)
{
    if (len < 16) return -1;
    uint8_t iv[12];
    iv_for(hs->n++, iv);
    size_t start = out->len;
    if (wa_buf_reserve(out, len) != 0) return -1;
    if (wa_aes_gcm_decrypt(hs->k, 32, iv, 12, hs->h, 32, ct, len, out->data + start) != 0) return -1;
    out->len += len - 16;
    mix_hash(hs, ct, len);
    return 0;
}

int wa_noise_start(wa_noise_hs *hs, wa_buf *out)
{
    memset(hs, 0, sizeof *hs);
    memcpy(hs->h, NOISE_XX, 32);
    memcpy(hs->ck, NOISE_XX, 32);
    mix_hash(hs, WA_CONN_HEADER, 4);
    if (wa_curve_keypair(hs->eph_priv, hs->eph_pub) != 0) return -1;
    mix_hash(hs, hs->eph_pub, 32);

    wa_buf hello;
    wa_buf_init(&hello);
    pb_put_bytes(&hello, 1, hs->eph_pub, 32);
    int rc = pb_put_message(out, 2, &hello);
    wa_buf_free(&hello);
    return rc;
}

// Checks the intermediate against WhatsApp's root and the leaf against the intermediate; the leaf key must be the
// static key the server just proved it holds.
static int verify_cert_chain(const uint8_t *chain, size_t len, const uint8_t server_static[32], char *err, size_t errlen)
{
    const uint8_t *leaf, *inter, *ldet, *lsig, *idet, *isig, *ikey, *lkey;
    size_t leaf_n, inter_n, ldet_n, lsig_n, idet_n, isig_n, ikey_n, lkey_n;
    if (!pb_get_bytes(chain, len, 1, &leaf, &leaf_n) || !pb_get_bytes(chain, len, 2, &inter, &inter_n) ||
        !pb_get_bytes(leaf, leaf_n, 1, &ldet, &ldet_n) || !pb_get_bytes(leaf, leaf_n, 2, &lsig, &lsig_n) ||
        !pb_get_bytes(inter, inter_n, 1, &idet, &idet_n) || !pb_get_bytes(inter, inter_n, 2, &isig, &isig_n)) {
        snprintf(err, errlen, "server certificate chain incomplete");
        return -1;
    }
    if (lsig_n != 64 || isig_n != 64) { snprintf(err, errlen, "server certificate signature size"); return -1; }
    if (wa_curve_verify(WA_CERT_PUB_KEY, idet, idet_n, isig) != 0) { snprintf(err, errlen, "intermediate certificate not signed by WhatsApp"); return -1; }
    uint64_t iserial = 0, issuer = 0, lissuer = 0, inot_after = 0, lnot_after = 0;
    pb_get_varint(idet, idet_n, 1, &iserial);
    pb_get_varint(idet, idet_n, 2, &issuer);
    pb_get_varint(idet, idet_n, 5, &inot_after);
    pb_get_varint(ldet, ldet_n, 2, &lissuer);
    pb_get_varint(ldet, ldet_n, 5, &lnot_after);
    if (issuer != 0) { snprintf(err, errlen, "intermediate issuer serial %u", (unsigned)issuer); return -1; }
    if (!pb_get_bytes(idet, idet_n, 3, &ikey, &ikey_n) || ikey_n != 32) { snprintf(err, errlen, "intermediate key"); return -1; }
    if (wa_curve_verify(ikey, ldet, ldet_n, lsig) != 0) { snprintf(err, errlen, "leaf certificate not signed by the intermediate"); return -1; }
    if (lissuer != iserial) { snprintf(err, errlen, "leaf issuer serial mismatch"); return -1; }
    if (!pb_get_bytes(ldet, ldet_n, 3, &lkey, &lkey_n) || lkey_n != 32 || memcmp(lkey, server_static, 32) != 0) {
        snprintf(err, errlen, "leaf certificate key is not the server's static key");
        return -1;
    }
    int64_t now = wa_unix_time();
    if ((inot_after && (int64_t)inot_after < now) || (lnot_after && (int64_t)lnot_after < now))
        wa_log("noise: server certificate expired by our clock (continuing)");
    return 0;
}

int wa_noise_finish(wa_noise_hs *hs, const uint8_t *server_hello, size_t len, const uint8_t static_priv[32],
                    const uint8_t static_pub[32], const uint8_t *client_payload, size_t payload_len, wa_buf *out,
                    char *err, size_t errlen)
{
    const uint8_t *sh, *eph, *stat, *pay;
    size_t sh_n, eph_n, stat_n, pay_n;
    if (!pb_get_bytes(server_hello, len, 3, &sh, &sh_n) || !pb_get_bytes(sh, sh_n, 1, &eph, &eph_n) || eph_n != 32 ||
        !pb_get_bytes(sh, sh_n, 2, &stat, &stat_n) || !pb_get_bytes(sh, sh_n, 3, &pay, &pay_n)) {
        snprintf(err, errlen, "malformed ServerHello");
        return -1;
    }
    mix_hash(hs, eph, 32);
    if (mix_dh(hs, hs->eph_priv, eph) != 0) { snprintf(err, errlen, "DH(e, re) failed"); return -1; }

    wa_buf plain;
    wa_buf_init(&plain);
    if (decrypt_and_hash(hs, stat, stat_n, &plain) != 0 || plain.len != 32) {
        wa_buf_free(&plain);
        snprintf(err, errlen, "cannot decrypt the server static key");
        return -1;
    }
    memcpy(hs->server_static, plain.data, 32);
    if (mix_dh(hs, hs->eph_priv, hs->server_static) != 0) { wa_buf_free(&plain); snprintf(err, errlen, "DH(e, rs) failed"); return -1; }

    wa_buf_clear(&plain);
    if (decrypt_and_hash(hs, pay, pay_n, &plain) != 0) {
        wa_buf_free(&plain);
        snprintf(err, errlen, "cannot decrypt the server certificate");
        return -1;
    }
    int cert = verify_cert_chain(plain.data, plain.len, hs->server_static, err, errlen);
    wa_buf_free(&plain);
    if (cert != 0) return -1;
    hs->cert_ok = 1;

    wa_buf enc_static, enc_payload, finish;
    wa_buf_init(&enc_static);
    wa_buf_init(&enc_payload);
    wa_buf_init(&finish);
    int rc = encrypt_and_hash(hs, static_pub, 32, &enc_static);
    if (rc == 0) rc = mix_dh(hs, static_priv, eph);
    if (rc == 0) rc = encrypt_and_hash(hs, client_payload, payload_len, &enc_payload);
    if (rc == 0) {
        pb_put_bytes(&finish, 1, enc_static.data, enc_static.len);
        pb_put_bytes(&finish, 2, enc_payload.data, enc_payload.len);
        rc = pb_put_message(out, 4, &finish);
    }
    wa_buf_free(&enc_static);
    wa_buf_free(&enc_payload);
    wa_buf_free(&finish);
    if (rc != 0) snprintf(err, errlen, "cannot build ClientFinish");
    return rc;
}

void wa_noise_split(wa_noise_hs *hs, wa_noise_cipher *write, wa_noise_cipher *read)
{
    uint8_t out[64];
    wa_hkdf_sha256(hs->ck, 32, NULL, 0, NULL, 0, out, 64);
    memcpy(write->key, out, 32);
    memcpy(read->key, out + 32, 32);
    write->counter = read->counter = 0;
    wa_wipe(out, sizeof out);
    wa_wipe(hs->eph_priv, 32);
}

int wa_noise_encrypt(wa_noise_cipher *c, const uint8_t *in, size_t len, wa_buf *out)
{
    uint8_t iv[12];
    iv_for(c->counter++, iv);
    size_t start = out->len;
    if (wa_buf_reserve(out, len + 16) != 0) return -1;
    if (wa_aes_gcm_encrypt(c->key, 32, iv, 12, NULL, 0, in, len, out->data + start) != 0) return -1;
    out->len += len + 16;
    return 0;
}

int wa_noise_decrypt(wa_noise_cipher *c, const uint8_t *in, size_t len, wa_buf *out)
{
    uint8_t iv[12];
    iv_for(c->counter++, iv);
    size_t start = out->len;
    if (len < 16 || wa_buf_reserve(out, len) != 0) return -1;
    if (wa_aes_gcm_decrypt(c->key, 32, iv, 12, NULL, 0, in, len, out->data + start) != 0) return -1;
    out->len += len - 16;
    return 0;
}

int wa_noise_frame(wa_buf *out, const uint8_t *data, size_t len, int first)
{
    if (len >= (1u << 24)) return -1;
    uint8_t hdr[3];
    wa_put_u24(hdr, (uint32_t)len);
    if (first && wa_buf_append(out, WA_CONN_HEADER, 4) != 0) return -1;
    if (wa_buf_append(out, hdr, 3) != 0) return -1;
    return wa_buf_append(out, data, len);
}

int wa_noise_unframe(wa_buf *in, wa_buf *out)
{
    if (in->len < 3) return 0;
    uint32_t n = wa_get_u24(in->data);
    if (in->len < 3 + (size_t)n) return 0;
    wa_buf_clear(out);
    if (wa_buf_append(out, in->data + 3, n) != 0) return -1;
    wa_buf_consume(in, 3 + (size_t)n);
    return 1;
}
