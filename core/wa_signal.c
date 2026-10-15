#include "wa_signal.h"
#include "wa_crypto.h"
#include "wa_proto.h"

#include <string.h>
#include <stdlib.h>

#define SIG_VERSION 0x33            // (3 << 4) | 3
#define MAC_LEN 8
#define MAX_FORWARD_JUMPS 2000

static void ser_pub(uint8_t out[33], const uint8_t pub[32]) { out[0] = 5; memcpy(out + 1, pub, 32); }

// ChainKey.step_with_message_keys
static void chain_step(const uint8_t chain[32], uint8_t mk_seed[32], uint8_t next_chain[32])
{
    uint8_t s1 = 0x01, s2 = 0x02;
    if (mk_seed) wa_hmac_sha256(chain, 32, &s1, 1, mk_seed);
    if (next_chain) wa_hmac_sha256(chain, 32, &s2, 1, next_chain);
}

// MessageKeys.derive_keys(mk_seed) → cipher(32), mac(32), iv(16)
static void msg_keys(const uint8_t mk_seed[32], uint8_t cipher[32], uint8_t mac[32], uint8_t iv[16])
{
    uint8_t okm[80];
    wa_hkdf_sha256(NULL, 0, mk_seed, 32, "WhisperMessageKeys", 18, okm, 80);
    memcpy(cipher, okm, 32);
    memcpy(mac, okm + 32, 32);
    memcpy(iv, okm + 64, 16);
    wa_wipe(okm, sizeof okm);
}

// RootKey.create_chain
static int root_create_chain(const uint8_t root[32], const uint8_t their_pub[32], const uint8_t our_priv[32],
                             uint8_t new_root[32], uint8_t chain[32])
{
    uint8_t shared[32], okm[64];
    if (wa_curve_dh(shared, our_priv, their_pub) != 0) return -1;
    wa_hkdf_sha256(root, 32, shared, 32, "WhisperRatchet", 14, okm, 64);
    memcpy(new_root, okm, 32);
    memcpy(chain, okm + 32, 32);
    wa_wipe(shared, sizeof shared);
    wa_wipe(okm, sizeof okm);
    return 0;
}

// X3DH: derive (root, chain) from the shared-secret concatenation
static void derive_x3dh(const uint8_t *secrets, size_t len, uint8_t root[32], uint8_t chain[32])
{
    uint8_t okm[96];
    wa_hkdf_sha256(NULL, 0, secrets, len, "WhisperText", 11, okm, 96);
    memcpy(root, okm, 32);
    memcpy(chain, okm + 32, 32);
    wa_wipe(okm, sizeof okm);
}

static void compute_mac(const uint8_t sender_id[32], const uint8_t recv_id[32], const uint8_t mac_key[32],
                        const uint8_t *serialized, size_t len, uint8_t out[MAC_LEN])
{
    uint8_t si[33], ri[33], full[32];
    ser_pub(si, sender_id);
    ser_pub(ri, recv_id);
    const void *parts[3] = { si, ri, serialized };
    size_t lens[3] = { 33, 33, len };
    wa_hmac_sha256_v(mac_key, 32, 3, parts, lens, full);
    memcpy(out, full, MAC_LEN);
}

// Serialize a SignalMessage body (version + proto) into out, then append the MAC. Returns 0 or -1.
static int build_signal_message(wa_sig_session *s, const uint8_t ratchet_pub[32], uint32_t counter, uint32_t prev,
                                const uint8_t *ciphertext, size_t ct_len, const uint8_t mac_key[32], wa_buf *out)
{
    size_t start = out->len;
    if (wa_buf_append_byte(out, SIG_VERSION) != 0) return -1;
    uint8_t rk[33];
    ser_pub(rk, ratchet_pub);
    if (pb_put_bytes(out, 1, rk, 33) != 0) return -1;
    pb_put_varint(out, 2, counter);
    pb_put_varint(out, 3, prev);
    pb_put_bytes(out, 4, ciphertext, ct_len);
    uint8_t mac[MAC_LEN];
    compute_mac(s->local_identity, s->remote_identity, mac_key, out->data + start, out->len - start, mac);
    return wa_buf_append(out, mac, MAC_LEN);
}

int wa_sig_init_outgoing(wa_sig_session *s, const wa_sig_keypair *our_identity, uint32_t our_reg_id, const wa_sig_bundle *b)
{
    // verify the signed pre key signature (over its 33-byte serialized form) with the peer identity key
    uint8_t spk[33];
    ser_pub(spk, b->signed_prekey_pub);
    if (wa_curve_verify(b->identity_pub, spk, 33, b->signed_prekey_sig) != 0) return -1;

    memset(s, 0, sizeof *s);
    s->version = 3;
    memcpy(s->local_identity, our_identity->pub, 32);
    memcpy(s->remote_identity, b->identity_pub, 32);

    wa_sig_keypair base;
    if (wa_curve_keypair(base.priv, base.pub) != 0) return -1;

    uint8_t secrets[160];
    size_t n = 0;
    memset(secrets, 0xFF, 32);
    n = 32;
    if (wa_curve_dh(secrets + n, our_identity->priv, b->signed_prekey_pub) != 0) return -1; n += 32;
    if (wa_curve_dh(secrets + n, base.priv, b->identity_pub) != 0) return -1; n += 32;
    if (wa_curve_dh(secrets + n, base.priv, b->signed_prekey_pub) != 0) return -1; n += 32;
    if (b->has_one_time) { if (wa_curve_dh(secrets + n, base.priv, b->one_time_pub) != 0) return -1; n += 32; }

    uint8_t root[32], chain[32];
    derive_x3dh(secrets, n, root, chain);
    wa_wipe(secrets, sizeof secrets);

    // initial receiver chain uses their signed pre key as the ratchet key
    s->recv[0].used = 1;
    memcpy(s->recv[0].ratchet, b->signed_prekey_pub, 32);
    memcpy(s->recv[0].chain, chain, 32);
    s->recv[0].index = 0;
    s->nrecv = 1;

    // sending chain: a fresh ratchet key, create_chain against their signed pre key
    if (wa_curve_keypair(s->send_ratchet.priv, s->send_ratchet.pub) != 0) return -1;
    uint8_t new_root[32];
    if (root_create_chain(root, b->signed_prekey_pub, s->send_ratchet.priv, new_root, s->send_chain) != 0) return -1;
    memcpy(s->root, new_root, 32);
    s->send_index = 0;
    s->has_send = 1;

    s->has_pending = 1;
    s->pending_reg_id = our_reg_id;
    s->pending_has_prekey = b->has_one_time;
    s->pending_prekey_id = b->prekey_id;
    s->pending_signed_prekey_id = b->signed_prekey_id;
    memcpy(s->pending_base_pub, base.pub, 32);
    s->established = 1;
    wa_wipe(base.priv, 32);
    return 0;
}

int wa_sig_encrypt(wa_sig_session *s, const wa_sig_keypair *our_identity, const uint8_t *pt, size_t ptlen,
                   int *is_prekey, wa_buf *out)
{
    (void)our_identity;
    if (!s->established || !s->has_send) return -1;
    uint8_t mk_seed[32], next_chain[32], cipher[32], mac_key[32], iv[16];
    chain_step(s->send_chain, mk_seed, next_chain);
    msg_keys(mk_seed, cipher, mac_key, iv);

    wa_buf ct;
    wa_buf_init(&ct);
    if (wa_aes_cbc_encrypt(cipher, iv, pt, ptlen, &ct) != 0) { wa_buf_free(&ct); return -1; }

    wa_buf inner;
    wa_buf_init(&inner);
    int rc = build_signal_message(s, s->send_ratchet.pub, s->send_index, s->previous_counter, ct.data, ct.len, mac_key, &inner);
    wa_buf_free(&ct);
    if (rc != 0) { wa_buf_free(&inner); return -1; }

    // advance the sending chain
    memcpy(s->send_chain, next_chain, 32);
    s->send_index++;
    wa_wipe(mk_seed, 32);
    wa_wipe(cipher, 32);
    wa_wipe(mac_key, 32);

    if (s->has_pending) {
        *is_prekey = 1;
        if (wa_buf_append_byte(out, SIG_VERSION) != 0) { wa_buf_free(&inner); return -1; }
        pb_put_varint(out, 5, s->pending_reg_id);
        if (s->pending_has_prekey) pb_put_varint(out, 1, s->pending_prekey_id);
        pb_put_varint(out, 6, s->pending_signed_prekey_id);
        uint8_t bk[33], ik[33];
        ser_pub(bk, s->pending_base_pub);
        ser_pub(ik, s->local_identity);
        pb_put_bytes(out, 2, bk, 33);
        pb_put_bytes(out, 3, ik, 33);
        pb_put_bytes(out, 4, inner.data, inner.len);
    } else {
        *is_prekey = 0;
        rc = wa_buf_append(out, inner.data, inner.len);
    }
    wa_buf_free(&inner);
    return rc;
}

int wa_sig_prekey_header(const uint8_t *msg, size_t len, uint32_t *reg_id, int *has_prekey, uint32_t *prekey_id,
                         uint32_t *signed_prekey_id, uint8_t base_pub[32], uint8_t identity_pub[32])
{
    if (len < 1 || msg[0] != SIG_VERSION) return -1;
    const uint8_t *body = msg + 1;
    size_t bl = len - 1;
    uint64_t v = 0;
    *reg_id = pb_get_varint(body, bl, 5, &v) ? (uint32_t)v : 0;
    *has_prekey = pb_get_varint(body, bl, 1, &v);
    *prekey_id = *has_prekey ? (uint32_t)v : 0;
    *signed_prekey_id = pb_get_varint(body, bl, 6, &v) ? (uint32_t)v : 0;
    const uint8_t *p;
    size_t pl;
    if (!pb_get_bytes(body, bl, 2, &p, &pl) || pl != 33) return -1;
    memcpy(base_pub, p + 1, 32);
    if (!pb_get_bytes(body, bl, 3, &p, &pl) || pl != 33) return -1;
    memcpy(identity_pub, p + 1, 32);
    return 0;
}

// Advance a receiver chain to `counter`, returning the message keys there; stores skipped keys in the session.
// Operates on a working copy of the chain so the caller can verify the MAC before committing.
static int derive_recv_keys(wa_sig_session *s, int chain_idx, uint32_t counter, uint8_t cipher[32], uint8_t mac[32], uint8_t iv[16],
                            int commit)
{
    wa_sig_recv_chain *rc = &s->recv[chain_idx];
    if (counter < rc->index) {
        // look in skipped keys
        for (int i = 0; i < s->nskipped; i++) {
            if (s->skipped[i].counter == counter && memcmp(s->skipped[i].ratchet, rc->ratchet, 32) == 0) {
                memcpy(cipher, s->skipped[i].cipher, 32);
                memcpy(mac, s->skipped[i].mac, 32);
                memcpy(iv, s->skipped[i].iv, 16);
                if (commit) {
                    s->skipped[i] = s->skipped[--s->nskipped];
                }
                return 0;
            }
        }
        return -1;   // already used / too old
    }
    if (counter - rc->index > MAX_FORWARD_JUMPS) return -1;

    uint8_t chain[32];
    memcpy(chain, rc->chain, 32);
    uint32_t idx = rc->index;
    uint8_t mk[32], next[32], c[32], m[32], v[16];
    while (idx < counter) {
        chain_step(chain, mk, next);
        if (commit && s->nskipped < WA_SIG_MAX_SKIPPED) {
            msg_keys(mk, c, m, v);
            wa_sig_skipped *sk = &s->skipped[s->nskipped++];
            memcpy(sk->ratchet, rc->ratchet, 32);
            sk->counter = idx;
            memcpy(sk->cipher, c, 32);
            memcpy(sk->mac, m, 32);
            memcpy(sk->iv, v, 16);
        }
        memcpy(chain, next, 32);
        idx++;
    }
    chain_step(chain, mk, next);
    msg_keys(mk, cipher, mac, iv);
    if (commit) {
        memcpy(rc->chain, next, 32);
        rc->index = counter + 1;
    }
    wa_wipe(mk, 32);
    wa_wipe(chain, 32);
    return 0;
}

// DH-ratchet step: a new receiver ratchet key arrived. Creates a receiver chain for it and a fresh sending chain.
static int dh_ratchet(wa_sig_session *s, const uint8_t their_ratchet[32])
{
    uint8_t new_root[32], recv_chain[32];
    if (root_create_chain(s->root, their_ratchet, s->send_ratchet.priv, new_root, recv_chain) != 0) return -1;

    // insert the receiver chain at the front
    if (s->nrecv < WA_SIG_MAX_RECV_CHAINS) s->nrecv++;
    for (int i = s->nrecv - 1; i > 0; i--) s->recv[i] = s->recv[i - 1];
    s->recv[0].used = 1;
    memcpy(s->recv[0].ratchet, their_ratchet, 32);
    memcpy(s->recv[0].chain, recv_chain, 32);
    s->recv[0].index = 0;

    // fresh sending chain
    wa_sig_keypair nk;
    if (wa_curve_keypair(nk.priv, nk.pub) != 0) return -1;
    uint8_t new_root2[32], send_chain[32];
    if (root_create_chain(new_root, their_ratchet, nk.priv, new_root2, send_chain) != 0) { wa_wipe(nk.priv, 32); return -1; }
    s->previous_counter = s->send_index;
    memcpy(s->root, new_root2, 32);
    s->send_ratchet = nk;
    memcpy(s->send_chain, send_chain, 32);
    s->send_index = 0;
    s->has_send = 1;
    wa_wipe(nk.priv, 32);
    return 0;
}

// Decrypt the inner SignalMessage bytes on the established session. 0 or -1.
static int decrypt_signal(wa_sig_session *s, const uint8_t *msg, size_t len, wa_buf *out)
{
    if (len < 1 + MAC_LEN || msg[0] != SIG_VERSION) return -1;
    const uint8_t *body = msg + 1;
    size_t bl = len - 1 - MAC_LEN;
    const uint8_t *rk, *ct;
    size_t rk_n, ct_n;
    uint64_t counter = 0;
    if (!pb_get_bytes(body, bl, 1, &rk, &rk_n) || rk_n != 33) return -1;
    if (!pb_get_varint(body, bl, 2, &counter)) return -1;
    if (!pb_get_bytes(body, bl, 4, &ct, &ct_n)) return -1;
    const uint8_t *their_ratchet = rk + 1;

    // find or create the receiver chain for this ratchet key
    int ci = -1;
    for (int i = 0; i < s->nrecv; i++) if (s->recv[i].used && memcmp(s->recv[i].ratchet, their_ratchet, 32) == 0) { ci = i; break; }
    if (ci < 0) {
        if (dh_ratchet(s, their_ratchet) != 0) return -1;
        ci = 0;
    }

    uint8_t cipher[32], mac_key[32], iv[16];
    if (derive_recv_keys(s, ci, (uint32_t)counter, cipher, mac_key, iv, 0) != 0) return -1;

    // verify MAC over msg[0 .. len-MAC_LEN] with sender=remote, receiver=local
    uint8_t expect[MAC_LEN];
    compute_mac(s->remote_identity, s->local_identity, mac_key, msg, len - MAC_LEN, expect);
    if (wa_ct_compare(expect, msg + len - MAC_LEN, MAC_LEN) != 0) return -1;

    // commit the chain advance now that the MAC is good
    derive_recv_keys(s, ci, (uint32_t)counter, cipher, mac_key, iv, 1);

    if (wa_aes_cbc_decrypt(cipher, iv, ct, ct_n, out) != 0) return -1;
    s->has_pending = 0;   // the peer is now talking to us
    wa_wipe(cipher, 32);
    wa_wipe(mac_key, 32);
    return 0;
}

int wa_sig_decrypt(wa_sig_session *s, const uint8_t *msg, size_t len, wa_buf *out)
{
    if (!s->established) return -1;
    return decrypt_signal(s, msg, len, out);
}

int wa_sig_decrypt_prekey(wa_sig_session *s, const wa_sig_keypair *our_identity,
                          const wa_sig_keypair *our_signed_prekey, const wa_sig_keypair *our_one_time,
                          const uint8_t *msg, size_t len, wa_buf *out)
{
    uint32_t reg = 0, prekey_id = 0, signed_id = 0;
    int has_prekey = 0;
    uint8_t their_base[32], their_identity[32];
    if (wa_sig_prekey_header(msg, len, &reg, &has_prekey, &prekey_id, &signed_id, their_base, their_identity) != 0) return -1;
    const uint8_t *inner;
    size_t inner_n;
    if (!pb_get_bytes(msg + 1, len - 1, 4, &inner, &inner_n)) return -1;

    memset(s, 0, sizeof *s);
    s->version = 3;
    memcpy(s->local_identity, our_identity->pub, 32);
    memcpy(s->remote_identity, their_identity, 32);

    uint8_t secrets[160];
    size_t n = 0;
    memset(secrets, 0xFF, 32);
    n = 32;
    if (wa_curve_dh(secrets + n, our_signed_prekey->priv, their_identity) != 0) return -1; n += 32;
    if (wa_curve_dh(secrets + n, our_identity->priv, their_base) != 0) return -1; n += 32;
    if (wa_curve_dh(secrets + n, our_signed_prekey->priv, their_base) != 0) return -1; n += 32;
    if (has_prekey && our_one_time) { if (wa_curve_dh(secrets + n, our_one_time->priv, their_base) != 0) return -1; n += 32; }

    uint8_t chain[32];
    derive_x3dh(secrets, n, s->root, chain);
    wa_wipe(secrets, sizeof secrets);

    // Bob's sending chain uses his signed pre key as the ratchet key
    s->send_ratchet = *our_signed_prekey;
    memcpy(s->send_chain, chain, 32);
    s->send_index = 0;
    s->has_send = 1;
    s->nrecv = 0;
    s->established = 1;

    return decrypt_signal(s, inner, inner_n, out);
}

// -------------------------------------------------------------------------------------------------- WA padding

int wa_sig_pad(const uint8_t *in, size_t len, wa_buf *out)
{
    uint8_t pad;
    wa_random(&pad, 1);
    pad = (uint8_t)((pad & 0x0F) + 1);   // 1..16
    if (wa_buf_append(out, in, len) != 0) return -1;
    for (int i = 0; i < pad; i++) if (wa_buf_append_byte(out, pad) != 0) return -1;
    return 0;
}

int wa_sig_unpad(const uint8_t *in, size_t len, wa_buf *out)
{
    if (len == 0) return -1;
    uint8_t pad = in[len - 1];
    if (pad == 0 || pad > 16 || pad > len) return -1;
    return wa_buf_append(out, in, len - pad);
}

// -------------------------------------------------------------------------------------------------- self test

static int roundtrip_msg(wa_sig_session *from, const wa_sig_keypair *from_id, wa_sig_session *to,
                         const char *text, int expect_prekey,
                         const wa_sig_keypair *bob_id, const wa_sig_keypair *bob_spk, const wa_sig_keypair *bob_otp)
{
    wa_buf padded, wire, got, plain;
    wa_buf_init(&padded);
    wa_buf_init(&wire);
    wa_buf_init(&got);
    wa_buf_init(&plain);
    int ok = 0, is_prekey = 0;
    if (wa_sig_pad((const uint8_t *)text, strlen(text), &padded) == 0 &&
        wa_sig_encrypt(from, from_id, padded.data, padded.len, &is_prekey, &wire) == 0 &&
        is_prekey == expect_prekey) {
        int drc;
        if (is_prekey && !to->established)
            drc = wa_sig_decrypt_prekey(to, bob_id, bob_spk, bob_otp, wire.data, wire.len, &got);
        else
            drc = wa_sig_decrypt(to, wire.data, wire.len, &got);
        if (drc == 0 && wa_sig_unpad(got.data, got.len, &plain) == 0 &&
            plain.len == strlen(text) && memcmp(plain.data, text, plain.len) == 0)
            ok = 1;
    }
    wa_buf_free(&padded);
    wa_buf_free(&wire);
    wa_buf_free(&got);
    wa_buf_free(&plain);
    return ok ? 0 : -1;
}

int wa_signal_selftest(void)
{
    // Alice and Bob identities; Bob publishes a signed pre key (+ one-time) that Alice fetches as a bundle.
    wa_sig_keypair alice_id, bob_id, bob_spk, bob_otp;
    if (wa_curve_keypair(alice_id.priv, alice_id.pub) || wa_curve_keypair(bob_id.priv, bob_id.pub) ||
        wa_curve_keypair(bob_spk.priv, bob_spk.pub) || wa_curve_keypair(bob_otp.priv, bob_otp.pub)) {
        wa_log("signal selftest: keygen");
        return -1;
    }
    wa_sig_bundle b;
    memset(&b, 0, sizeof b);
    b.registration_id = 4242;
    memcpy(b.identity_pub, bob_id.pub, 32);
    b.signed_prekey_id = 7;
    memcpy(b.signed_prekey_pub, bob_spk.pub, 32);
    uint8_t spk_ser[33];
    ser_pub(spk_ser, bob_spk.pub);
    if (wa_curve_sign(b.signed_prekey_sig, bob_id.priv, spk_ser, 33) != 0) { wa_log("signal selftest: sign spk"); return -1; }
    b.has_one_time = 1;
    b.prekey_id = 31;
    memcpy(b.one_time_pub, bob_otp.pub, 32);

    wa_sig_session alice, bob;
    memset(&bob, 0, sizeof bob);
    if (wa_sig_init_outgoing(&alice, &alice_id, 1001, &b) != 0) { wa_log("signal selftest: alice init (sig verify?)"); return -1; }

    // Alice → Bob (prekey), Bob → Alice (msg), then several more both ways, plus out-of-order.
    if (roundtrip_msg(&alice, &alice_id, &bob, "ahoj bobe", 1, &bob_id, &bob_spk, &bob_otp) != 0) { wa_log("signal selftest: A->B prekey"); return -1; }
    if (roundtrip_msg(&bob, &bob_id, &alice, "ahoj alice", 0, NULL, NULL, NULL) != 0) { wa_log("signal selftest: B->A"); return -1; }
    if (roundtrip_msg(&alice, &alice_id, &bob, "jak se mas", 0, NULL, NULL, NULL) != 0) { wa_log("signal selftest: A->B 2"); return -1; }
    if (roundtrip_msg(&bob, &bob_id, &alice, "dobre dik", 0, NULL, NULL, NULL) != 0) { wa_log("signal selftest: B->A 2"); return -1; }

    // out-of-order: Bob encrypts two, Alice decrypts the second then the first (skipped-key path)
    wa_buf p1, p2, w1, w2, g1, g2, u1, u2;
    wa_buf_init(&p1); wa_buf_init(&p2); wa_buf_init(&w1); wa_buf_init(&w2);
    wa_buf_init(&g1); wa_buf_init(&g2); wa_buf_init(&u1); wa_buf_init(&u2);
    int ip = 0, ok = 0;
    wa_sig_pad((const uint8_t *)"prvni", 5, &p1);
    wa_sig_pad((const uint8_t *)"druhy", 5, &p2);
    if (wa_sig_encrypt(&bob, &bob_id, p1.data, p1.len, &ip, &w1) == 0 &&
        wa_sig_encrypt(&bob, &bob_id, p2.data, p2.len, &ip, &w2) == 0 &&
        wa_sig_decrypt(&alice, w2.data, w2.len, &g2) == 0 &&
        wa_sig_decrypt(&alice, w1.data, w1.len, &g1) == 0 &&
        wa_sig_unpad(g2.data, g2.len, &u2) == 0 && u2.len == 5 && memcmp(u2.data, "druhy", 5) == 0 &&
        wa_sig_unpad(g1.data, g1.len, &u1) == 0 && u1.len == 5 && memcmp(u1.data, "prvni", 5) == 0)
        ok = 1;
    wa_buf_free(&p1); wa_buf_free(&p2); wa_buf_free(&w1); wa_buf_free(&w2);
    wa_buf_free(&g1); wa_buf_free(&g2); wa_buf_free(&u1); wa_buf_free(&u2);
    if (!ok) { wa_log("signal selftest: out-of-order skipped keys"); return -1; }
    return 0;
}
