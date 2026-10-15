// Cryptographic primitives used by the WhatsApp protocol: SHA-2, HMAC, HKDF, AES-GCM/CBC/CTR (mbedTLS) and
// Curve25519 key agreement + XEdDSA-style signatures in the Signal flavour (monocypher primitives).
#ifndef WA_CRYPTO_H
#define WA_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include "wa_util.h"

#ifdef __cplusplus
extern "C" {
#endif

void wa_sha256(const void *data, size_t len, uint8_t out[32]);
void wa_sha256_2(const void *a, size_t alen, const void *b, size_t blen, uint8_t out[32]);
void wa_sha512(const void *data, size_t len, uint8_t out[64]);
void wa_md5(const void *data, size_t len, uint8_t out[16]);

void wa_hmac_sha256(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t out[32]);
// HMAC-SHA256 over several parts (n pairs of pointer/length)
void wa_hmac_sha256_v(const uint8_t *key, size_t klen, int n, const void *const *parts, const size_t *lens, uint8_t out[32]);
void wa_hmac_sha1(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t out[20]);
void wa_hmac_sha1_2(const uint8_t *key, size_t klen, const void *a, size_t alen, const void *b, size_t blen, uint8_t out[20]);
void wa_hmac_sha512(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t out[64]);

// HKDF-SHA256 (RFC 5869). salt may be NULL (zeros). Returns 0 or -1.
int wa_hkdf_sha256(const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, const void *info, size_t infolen, uint8_t *out, size_t outlen);

// AES-GCM with a 16-byte tag appended to the ciphertext. key is 16 or 32 bytes.
int wa_aes_gcm_encrypt(const uint8_t *key, size_t keylen, const uint8_t *iv, size_t ivlen, const uint8_t *aad, size_t aadlen,
                       const uint8_t *in, size_t len, uint8_t *out /* len + 16 */);
// Returns 0, or -1 when the tag does not verify. in includes the tag; out gets len - 16 bytes.
int wa_aes_gcm_decrypt(const uint8_t *key, size_t keylen, const uint8_t *iv, size_t ivlen, const uint8_t *aad, size_t aadlen,
                       const uint8_t *in, size_t len, uint8_t *out);

// AES-256-CBC with PKCS#7 padding. Append to out. 0 or -1 (bad padding / length).
int wa_aes_cbc_encrypt(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t len, wa_buf *out);
int wa_aes_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t len, wa_buf *out);

// AES-CTR (key 16 or 32 bytes); iv is the full 16-byte initial counter block. in and out may alias.
int wa_aes_ctr(const uint8_t *key, size_t keylen, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len);
// One AES block (ECB encrypt) - for the SRTP keystream helpers
int wa_aes_ecb_encrypt(const uint8_t *key, size_t keylen, const uint8_t in[16], uint8_t out[16]);

// Curve25519. Private keys are stored clamped. Public keys are the raw 32 bytes (the Signal 0x05 type byte is
// added by the callers that need it).
int wa_curve_keypair(uint8_t priv[32], uint8_t pub[32]);
void wa_curve_public(const uint8_t priv[32], uint8_t pub[32]);
int wa_curve_dh(uint8_t out[32], const uint8_t priv[32], const uint8_t pub[32]);   // -1 when the result is all zero
// Signal's Curve25519 signatures (XEdDSA with the Edwards sign bit stored in the top bit of the signature)
int wa_curve_sign(uint8_t sig[64], const uint8_t priv[32], const void *msg, size_t len);
int wa_curve_verify(const uint8_t pub[32], const void *msg, size_t len, const uint8_t sig[64]);   // 0 = valid

int wa_crypto_selftest(void);   // 0 when the built-in vectors pass

#ifdef __cplusplus
}
#endif
#endif
