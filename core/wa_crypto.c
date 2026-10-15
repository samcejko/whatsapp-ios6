#include "wa_crypto.h"

#include <string.h>
#include <stdlib.h>

#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "mbedtls/md5.h"
#include "mbedtls/md.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/gcm.h"
#include "mbedtls/aes.h"
#include "monocypher.h"

void wa_sha256(const void *data, size_t len, uint8_t out[32])
{
    mbedtls_sha256((const unsigned char *)data, len, out, 0);
}

void wa_sha256_2(const void *a, size_t alen, const void *b, size_t blen, uint8_t out[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, (const unsigned char *)a, alen);
    mbedtls_sha256_update(&c, (const unsigned char *)b, blen);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

void wa_sha512(const void *data, size_t len, uint8_t out[64])
{
    mbedtls_sha512((const unsigned char *)data, len, out, 0);
}

void wa_md5(const void *data, size_t len, uint8_t out[16])
{
    mbedtls_md5((const unsigned char *)data, len, out);
}

static void hmac(mbedtls_md_type_t type, const uint8_t *key, size_t klen, int n, const void *const *parts, const size_t *lens, uint8_t *out)
{
    mbedtls_md_context_t c;
    mbedtls_md_init(&c);
    mbedtls_md_setup(&c, mbedtls_md_info_from_type(type), 1);
    mbedtls_md_hmac_starts(&c, key, klen);
    for (int i = 0; i < n; i++) mbedtls_md_hmac_update(&c, (const unsigned char *)parts[i], lens[i]);
    mbedtls_md_hmac_finish(&c, out);
    mbedtls_md_free(&c);
}

void wa_hmac_sha256(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t out[32])
{
    const void *p[1] = { data };
    size_t l[1] = { len };
    hmac(MBEDTLS_MD_SHA256, key, klen, 1, p, l, out);
}

void wa_hmac_sha256_v(const uint8_t *key, size_t klen, int n, const void *const *parts, const size_t *lens, uint8_t out[32])
{
    hmac(MBEDTLS_MD_SHA256, key, klen, n, parts, lens, out);
}

void wa_hmac_sha1(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t out[20])
{
    const void *p[1] = { data };
    size_t l[1] = { len };
    hmac(MBEDTLS_MD_SHA1, key, klen, 1, p, l, out);
}

void wa_hmac_sha1_2(const uint8_t *key, size_t klen, const void *a, size_t alen, const void *b, size_t blen, uint8_t out[20])
{
    const void *p[2] = { a, b };
    size_t l[2] = { alen, blen };
    hmac(MBEDTLS_MD_SHA1, key, klen, 2, p, l, out);
}

void wa_hmac_sha512(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t out[64])
{
    const void *p[1] = { data };
    size_t l[1] = { len };
    hmac(MBEDTLS_MD_SHA512, key, klen, 1, p, l, out);
}

int wa_hkdf_sha256(const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, const void *info, size_t infolen, uint8_t *out, size_t outlen)
{
    static const uint8_t zero = 0;
    return mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, slen, ikm ? ikm : &zero, ilen,
                        (const unsigned char *)info, infolen, out, outlen) == 0 ? 0 : -1;
}

int wa_aes_gcm_encrypt(const uint8_t *key, size_t keylen, const uint8_t *iv, size_t ivlen, const uint8_t *aad, size_t aadlen,
                       const uint8_t *in, size_t len, uint8_t *out)
{
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, (unsigned)(keylen * 8));
    if (rc == 0) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len, iv, ivlen, aad, aadlen, in, out, 16, out + len);
    mbedtls_gcm_free(&g);
    return rc == 0 ? 0 : -1;
}

int wa_aes_gcm_decrypt(const uint8_t *key, size_t keylen, const uint8_t *iv, size_t ivlen, const uint8_t *aad, size_t aadlen,
                       const uint8_t *in, size_t len, uint8_t *out)
{
    if (len < 16) return -1;
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, (unsigned)(keylen * 8));
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&g, len - 16, iv, ivlen, aad, aadlen, in + len - 16, 16, in, out);
    mbedtls_gcm_free(&g);
    return rc == 0 ? 0 : -1;
}

int wa_aes_cbc_encrypt(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t len, wa_buf *out)
{
    size_t pad = 16 - len % 16, total = len + pad;
    if (wa_buf_reserve(out, total) != 0) return -1;
    uint8_t *dst = out->data + out->len;
    memcpy(dst, in, len);
    memset(dst + len, (int)pad, pad);
    uint8_t ivc[16];
    memcpy(ivc, iv, 16);
    mbedtls_aes_context a;
    mbedtls_aes_init(&a);
    int rc = mbedtls_aes_setkey_enc(&a, key, 256);
    if (rc == 0) rc = mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_ENCRYPT, total, ivc, dst, dst);
    mbedtls_aes_free(&a);
    if (rc != 0) return -1;
    out->len += total;
    return 0;
}

int wa_aes_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t len, wa_buf *out)
{
    if (len == 0 || len % 16) return -1;
    if (wa_buf_reserve(out, len) != 0) return -1;
    uint8_t *dst = out->data + out->len;
    uint8_t ivc[16];
    memcpy(ivc, iv, 16);
    mbedtls_aes_context a;
    mbedtls_aes_init(&a);
    int rc = mbedtls_aes_setkey_dec(&a, key, 256);
    if (rc == 0) rc = mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_DECRYPT, len, ivc, in, dst);
    mbedtls_aes_free(&a);
    if (rc != 0) return -1;
    uint8_t pad = dst[len - 1];
    if (pad == 0 || pad > 16) return -1;
    for (size_t i = 0; i < pad; i++) if (dst[len - 1 - i] != pad) return -1;
    out->len += len - pad;
    return 0;
}

int wa_aes_ctr(const uint8_t *key, size_t keylen, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t nonce[16], stream[16];
    size_t off = 0;
    memcpy(nonce, iv, 16);
    mbedtls_aes_context a;
    mbedtls_aes_init(&a);
    int rc = mbedtls_aes_setkey_enc(&a, key, (unsigned)(keylen * 8));
    if (rc == 0) rc = mbedtls_aes_crypt_ctr(&a, len, &off, nonce, stream, in, out);
    mbedtls_aes_free(&a);
    return rc == 0 ? 0 : -1;
}

int wa_aes_ecb_encrypt(const uint8_t *key, size_t keylen, const uint8_t in[16], uint8_t out[16])
{
    mbedtls_aes_context a;
    mbedtls_aes_init(&a);
    int rc = mbedtls_aes_setkey_enc(&a, key, (unsigned)(keylen * 8));
    if (rc == 0) rc = mbedtls_aes_crypt_ecb(&a, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&a);
    return rc == 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------------------------------- Curve25519

int wa_curve_keypair(uint8_t priv[32], uint8_t pub[32])
{
    if (wa_random(priv, 32) != 0) return -1;
    priv[0] &= 248;
    priv[31] &= 127;
    priv[31] |= 64;
    crypto_x25519_public_key(pub, priv);
    return 0;
}

void wa_curve_public(const uint8_t priv[32], uint8_t pub[32])
{
    crypto_x25519_public_key(pub, priv);
}

int wa_curve_dh(uint8_t out[32], const uint8_t priv[32], const uint8_t pub[32])
{
    static const uint8_t zero[32];
    crypto_x25519(out, priv, pub);
    return wa_ct_compare(out, zero, 32) == 0 ? -1 : 0;
}

// Signal's calculate_signature: a = k mod L, A = aB (sign bit kept in the signature), r = H(0xFE 0xFF*31 || k || M || Z),
// R = rB, h = H(R || A || M), s = h*a + r.
int wa_curve_sign(uint8_t sig[64], const uint8_t priv[32], const void *msg, size_t len)
{
    static const uint8_t prefix[32] = { 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t wide[64], a[32], A[32], r[32], R[32], h[32], hash[64], z[64];
    if (wa_random(z, sizeof z) != 0) return -1;

    memcpy(wide, priv, 32);
    memset(wide + 32, 0, 32);
    crypto_eddsa_reduce(a, wide);
    crypto_eddsa_scalarbase(A, a);
    uint8_t sign_bit = A[31] & 0x80;

    mbedtls_sha512_context c;
    mbedtls_sha512_init(&c);
    mbedtls_sha512_starts(&c, 0);
    mbedtls_sha512_update(&c, prefix, 32);
    mbedtls_sha512_update(&c, priv, 32);
    mbedtls_sha512_update(&c, (const unsigned char *)msg, len);
    mbedtls_sha512_update(&c, z, 64);
    mbedtls_sha512_finish(&c, hash);
    crypto_eddsa_reduce(r, hash);
    crypto_eddsa_scalarbase(R, r);

    mbedtls_sha512_starts(&c, 0);
    mbedtls_sha512_update(&c, R, 32);
    mbedtls_sha512_update(&c, A, 32);
    mbedtls_sha512_update(&c, (const unsigned char *)msg, len);
    mbedtls_sha512_finish(&c, hash);
    mbedtls_sha512_free(&c);
    crypto_eddsa_reduce(h, hash);

    memcpy(sig, R, 32);
    crypto_eddsa_mul_add(sig + 32, h, a, r);
    sig[63] &= 0x7F;
    sig[63] |= sign_bit;

    wa_wipe(wide, sizeof wide);
    wa_wipe(a, sizeof a);
    wa_wipe(r, sizeof r);
    wa_wipe(z, sizeof z);
    return 0;
}

int wa_curve_verify(const uint8_t pub[32], const void *msg, size_t len, const uint8_t sig[64])
{
    uint8_t A[32], s[64], h[32], hash[64];
    crypto_x25519_to_eddsa(A, pub);
    A[31] = (uint8_t)((A[31] & 0x7F) | (sig[63] & 0x80));
    memcpy(s, sig, 64);
    s[63] &= 0x7F;
    if (s[63] & 0xE0) return -1;

    mbedtls_sha512_context c;
    mbedtls_sha512_init(&c);
    mbedtls_sha512_starts(&c, 0);
    mbedtls_sha512_update(&c, s, 32);
    mbedtls_sha512_update(&c, A, 32);
    mbedtls_sha512_update(&c, (const unsigned char *)msg, len);
    mbedtls_sha512_finish(&c, hash);
    mbedtls_sha512_free(&c);
    crypto_eddsa_reduce(h, hash);
    return crypto_eddsa_check_equation(s, A, h) == 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------------------------------- self test

int wa_crypto_selftest(void)
{
    // libsignal's test_signature vector (Curve25519 public key, XEdDSA signature over a 33-byte message)
    static const uint8_t alice_priv[32] = {
        0xc0, 0x97, 0x24, 0x84, 0x12, 0xe5, 0x8b, 0xf0, 0x5d, 0xf4, 0x87, 0x96, 0x82, 0x05, 0x13, 0x27,
        0x94, 0x17, 0x8e, 0x36, 0x76, 0x37, 0xf5, 0x81, 0x8f, 0x81, 0xe0, 0xe6, 0xce, 0x73, 0xe8, 0x65 };
    static const uint8_t alice_pub[32] = {
        0xab, 0x7e, 0x71, 0x7d, 0x4a, 0x16, 0x3b, 0x7d, 0x9a, 0x1d, 0x80, 0x71, 0xdf, 0xe9, 0xdc, 0xf8,
        0xcd, 0xcd, 0x1c, 0xea, 0x33, 0x39, 0xb6, 0x35, 0x6b, 0xe8, 0x4d, 0x88, 0x7e, 0x32, 0x2c, 0x64 };
    static const uint8_t msg[33] = {
        0x05, 0xed, 0xce, 0x9d, 0x9c, 0x41, 0x5c, 0xa7, 0x8c, 0xb7, 0x25, 0x2e, 0x72, 0xc2, 0xc4, 0xa5,
        0x54, 0xd3, 0xeb, 0x29, 0x48, 0x5a, 0x0e, 0x1d, 0x50, 0x31, 0x18, 0xd1, 0xa8, 0x2d, 0x99, 0xfb, 0x4a };
    static const uint8_t sig[64] = {
        0x5d, 0xe8, 0x8c, 0xa9, 0xa8, 0x9b, 0x4a, 0x11, 0x5d, 0xa7, 0x91, 0x09, 0xc6, 0x7c, 0x9c, 0x74,
        0x64, 0xa3, 0xe4, 0x18, 0x02, 0x74, 0xf1, 0xcb, 0x8c, 0x63, 0xc2, 0x98, 0x4e, 0x28, 0x6d, 0xfb,
        0xed, 0xe8, 0x2d, 0xeb, 0x9d, 0xcd, 0x9f, 0xae, 0x0b, 0xfb, 0xb8, 0x21, 0x56, 0x9b, 0x3d, 0x90,
        0x01, 0xbd, 0x81, 0x30, 0xcd, 0x11, 0xd4, 0x86, 0xce, 0xf0, 0x47, 0xbd, 0x60, 0xb8, 0x6e, 0x88 };
    uint8_t pub[32], mysig[64], bad[64];
    wa_curve_public(alice_priv, pub);
    if (memcmp(pub, alice_pub, 32) != 0) { wa_log("selftest: curve public key mismatch"); return -1; }
    if (wa_curve_verify(alice_pub, msg, sizeof msg, sig) != 0) { wa_log("selftest: libsignal signature rejected"); return -1; }
    memcpy(bad, sig, 64);
    bad[10] ^= 1;
    if (wa_curve_verify(alice_pub, msg, sizeof msg, bad) == 0) { wa_log("selftest: corrupted signature accepted"); return -1; }
    if (wa_curve_sign(mysig, alice_priv, msg, sizeof msg) != 0 || wa_curve_verify(alice_pub, msg, sizeof msg, mysig) != 0) {
        wa_log("selftest: own signature rejected");
        return -1;
    }

    // RFC 5869 test case 1
    static const uint8_t ikm[22] = { 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                                     0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b };
    static const uint8_t salt[13] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    static const uint8_t info[10] = { 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9 };
    static const uint8_t okm_expect[8] = { 0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a };
    uint8_t okm[42];
    if (wa_hkdf_sha256(salt, sizeof salt, ikm, sizeof ikm, info, sizeof info, okm, sizeof okm) != 0 || memcmp(okm, okm_expect, 8) != 0) {
        wa_log("selftest: HKDF mismatch");
        return -1;
    }

    // Curve25519 agreement both ways
    uint8_t a_priv[32], a_pub[32], b_priv[32], b_pub[32], s1[32], s2[32];
    wa_curve_keypair(a_priv, a_pub);
    wa_curve_keypair(b_priv, b_pub);
    wa_curve_dh(s1, a_priv, b_pub);
    wa_curve_dh(s2, b_priv, a_pub);
    if (memcmp(s1, s2, 32) != 0) { wa_log("selftest: DH mismatch"); return -1; }

    // AES-GCM and CBC round trips
    uint8_t key[32], iv[16], ct[48], pt[32];
    wa_random(key, 32);
    wa_random(iv, 16);
    memcpy(pt, "0123456789abcdef0123456789abcdef", 32);
    wa_aes_gcm_encrypt(key, 32, iv, 12, NULL, 0, pt, 32, ct);
    uint8_t back[32];
    if (wa_aes_gcm_decrypt(key, 32, iv, 12, NULL, 0, ct, 48, back) != 0 || memcmp(back, pt, 32) != 0) { wa_log("selftest: GCM"); return -1; }
    ct[3] ^= 1;
    if (wa_aes_gcm_decrypt(key, 32, iv, 12, NULL, 0, ct, 48, back) == 0) { wa_log("selftest: GCM accepted a bad tag"); return -1; }
    wa_buf e, d;
    wa_buf_init(&e);
    wa_buf_init(&d);
    wa_aes_cbc_encrypt(key, iv, pt, 20, &e);
    int cbc_ok = e.len == 32 && wa_aes_cbc_decrypt(key, iv, e.data, e.len, &d) == 0 && d.len == 20 && memcmp(d.data, pt, 20) == 0;
    wa_buf_free(&e);
    wa_buf_free(&d);
    if (!cbc_ok) { wa_log("selftest: CBC"); return -1; }
    return 0;
}
