#include "wa_srtp.h"
#include "wa_crypto.h"
#include "wa_util.h"

#include <string.h>

// AES-CM PRF (libsrtp KDF): IV = master_salt(14) zero-padded to 16, label XORed into byte 7, then AES-128-CTR
// (full 128-bit big-endian counter) keystream over `len` zero bytes.
static int aes_cm_kdf(const uint8_t master_key[16], const uint8_t master_salt[14], uint8_t label, uint8_t *out, size_t len)
{
    uint8_t iv[16];
    memset(iv, 0, sizeof iv);
    memcpy(iv, master_salt, 14);
    iv[7] ^= label;
    memset(out, 0, len);
    return wa_aes_ctr(master_key, 16, iv, out, out, len);
}

static int derive_keys(wa_srtp_keys *out, const uint8_t *call_key, size_t call_key_len, const char *lid,
                       uint8_t enc_label, uint8_t auth_label, uint8_t salt_label)
{
    if (call_key_len < 32) return -1;
    static const uint8_t zero_salt[32] = { 0 };
    uint8_t master[46];
    if (wa_hkdf_sha256(zero_salt, 32, call_key, 32, lid, strlen(lid), master, sizeof master) != 0) return -1;
    const uint8_t *mk = master, *ms = master + 16;   // master_key[0:16], master_salt[16:30]
    int rc = aes_cm_kdf(mk, ms, enc_label, out->cipher_key, 16);
    if (rc == 0) rc = aes_cm_kdf(mk, ms, auth_label, out->auth_key, 20);
    if (rc == 0) rc = aes_cm_kdf(mk, ms, salt_label, out->salt, 14);
    wa_wipe(master, sizeof master);
    return rc;
}

int wa_srtp_derive_e2e(wa_srtp_keys *out, const uint8_t *call_key, size_t call_key_len, const char *participant_lid)
{
    return derive_keys(out, call_key, call_key_len, participant_lid, 0x00, 0x01, 0x02);
}

int wa_srtp_derive_e2e_rtcp(wa_srtp_keys *out, const uint8_t *call_key, size_t call_key_len, const char *participant_lid)
{
    return derive_keys(out, call_key, call_key_len, participant_lid, 0x03, 0x04, 0x05);
}

void wa_srtp_rtp_iv(const wa_srtp_keys *k, uint32_t ssrc, uint32_t roc, uint16_t seq, uint8_t iv[16])
{
    memset(iv, 0, 16);
    memcpy(iv, k->salt, 14);                 // salt left-aligned into bytes [0..14]
    iv[4] ^= (uint8_t)(ssrc >> 24);
    iv[5] ^= (uint8_t)(ssrc >> 16);
    iv[6] ^= (uint8_t)(ssrc >> 8);
    iv[7] ^= (uint8_t)ssrc;
    uint64_t packet_index = (uint64_t)roc * 0x10000u + seq;   // 48-bit
    uint16_t hi16 = (uint16_t)((packet_index >> 32) & 0xFFFF);
    uint32_t lo32 = (uint32_t)(packet_index & 0xFFFFFFFFu);
    iv[8] ^= (uint8_t)(hi16 >> 8);
    iv[9] ^= (uint8_t)hi16;
    iv[10] ^= (uint8_t)(lo32 >> 24);
    iv[11] ^= (uint8_t)(lo32 >> 16);
    iv[12] ^= (uint8_t)(lo32 >> 8);
    iv[13] ^= (uint8_t)lo32;
}

int wa_srtp_crypt_payload(const wa_srtp_keys *k, uint32_t ssrc, uint32_t roc, uint16_t seq, uint8_t *data, size_t len)
{
    uint8_t iv[16];
    wa_srtp_rtp_iv(k, ssrc, roc, seq, iv);
    return wa_aes_ctr(k->cipher_key, 16, iv, data, data, len);
}

void wa_srtp_warp_tag(const uint8_t auth_key[20], const uint8_t *packet, size_t len, uint32_t roc, uint8_t tag[WA_WARP_TAG_LEN])
{
    uint8_t roc_be[4];
    wa_put_u32(roc_be, roc);
    uint8_t full[20];
    wa_hmac_sha1_2(auth_key, 20, packet, len, roc_be, 4, full);
    memcpy(tag, full, WA_WARP_TAG_LEN);
}

int wa_srtp_warp_verify(const uint8_t auth_key[20], const uint8_t *packet, size_t len, uint32_t roc,
                        const uint8_t *received_tag, size_t tag_len)
{
    if (tag_len != WA_WARP_TAG_LEN) return -1;
    uint8_t tag[WA_WARP_TAG_LEN];
    wa_srtp_warp_tag(auth_key, packet, len, roc, tag);
    return wa_ct_compare(tag, received_tag, WA_WARP_TAG_LEN) == 0 ? 0 : -1;
}

uint32_t wa_roc_send_advance(wa_roc_send *t, uint16_t seq)
{
    if (!t->started) { t->last_seq = seq; t->started = 1; return t->roc; }
    if ((int32_t)seq - (int32_t)t->last_seq < -32768) t->roc++;
    t->last_seq = seq;
    return t->roc;
}

uint32_t wa_roc_recv_estimate(const wa_roc_recv *t, uint16_t seq)
{
    if (!t->started) return t->roc;
    if (t->s_l < 0x8000) {
        if ((int32_t)seq - (int32_t)t->s_l > 0x8000) return t->roc - 1;
        return t->roc;
    }
    if ((int32_t)t->s_l - (int32_t)seq > 0x8000) return t->roc + 1;
    return t->roc;
}

void wa_roc_recv_commit(wa_roc_recv *t, uint32_t v, uint16_t seq)
{
    if (!t->started) { t->s_l = seq; t->started = 1; return; }
    if (v == t->roc) { if (seq > t->s_l) t->s_l = seq; }
    else if (v == t->roc + 1) { t->roc = v; t->s_l = seq; }
}

// -------------------------------------------------------------------------------------------------- self test

static int eqhex(const char *label, const uint8_t *got, const char *expect_hex)
{
    size_t n = strlen(expect_hex) / 2;
    uint8_t exp[64];
    if (n > sizeof exp) return -1;
    wa_hex_decode(expect_hex, exp, sizeof exp);
    if (memcmp(got, exp, n) != 0) {
        char hex[129];
        wa_hex_encode(got, n < 64 ? n : 64, hex, 0);
        wa_log("srtp selftest: %s mismatch (got %s, want %s)", label, hex, expect_hex);
        return -1;
    }
    return 0;
}

int wa_srtp_selftest(void)
{
    // meowcaller/whatsapp-rust testdata/kats.json
    static const uint8_t call_key[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f };
    const char *peer_lid = "222222222222222:0@lid";
    const char *self_lid = "111111111111111:0@lid";
    const uint32_t ssrc = 305419896u, roc = 0;
    const uint16_t seq = 7;
    static const uint8_t payload[12] = { 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xa9, 0xba, 0xcb };
    static const uint8_t sample_packet[16] = { 0x90, 0x78, 0x00, 0x07, 0x00, 0x00, 0x03, 0xc0, 0x12, 0x34, 0x56, 0x78, 0xde, 0xad, 0xbe, 0xef };

    wa_srtp_keys peer, me;
    if (wa_srtp_derive_e2e(&peer, call_key, 32, peer_lid) != 0 || wa_srtp_derive_e2e(&me, call_key, 32, self_lid) != 0) {
        wa_log("srtp selftest: key derivation failed");
        return -1;
    }
    int ok = 0;
    ok |= eqhex("peer_cipherKey", peer.cipher_key, "747055a9d18be0fcdc0e29d871309081");
    ok |= eqhex("peer_salt", peer.salt, "10fb8133eff7639cc0d06788f49c");
    ok |= eqhex("peer_authKey", peer.auth_key, "8eff4bb04971d92512b034ce0ebc466059bfc6ea");
    ok |= eqhex("self_cipherKey", me.cipher_key, "261bde5b556020e3e6da0f5e3132db57");
    ok |= eqhex("self_salt", me.salt, "44224a8c019b379c4bbf7dda284a");
    ok |= eqhex("self_authKey", me.auth_key, "56b2655b60a1c17c03dfe94950d5df233481ad41");

    uint8_t iv[16];
    wa_srtp_rtp_iv(&peer, ssrc, roc, seq, iv);
    ok |= eqhex("rtpIv", iv, "10fb8133fdc335e4c0d06788f49b0000");

    uint8_t ct[12];
    memcpy(ct, payload, 12);
    wa_srtp_crypt_payload(&peer, ssrc, roc, seq, ct, 12);
    ok |= eqhex("cipher_out", ct, "17b30591a2650ed7bbae9c5a");
    // symmetric round trip
    wa_srtp_crypt_payload(&peer, ssrc, roc, seq, ct, 12);
    if (memcmp(ct, payload, 12) != 0) { wa_log("srtp selftest: CTR not symmetric"); ok |= 1; }

    uint8_t tag[WA_WARP_TAG_LEN];
    wa_srtp_warp_tag(peer.auth_key, sample_packet, sizeof sample_packet, roc, tag);
    ok |= eqhex("warp_mi_tag4", tag, "53fada83");
    if (wa_srtp_warp_verify(peer.auth_key, sample_packet, sizeof sample_packet, roc, tag, WA_WARP_TAG_LEN) != 0) { wa_log("srtp selftest: warp verify"); ok |= 1; }
    uint8_t bad[WA_WARP_TAG_LEN];
    memcpy(bad, tag, WA_WARP_TAG_LEN);
    bad[0] ^= 1;
    if (wa_srtp_warp_verify(peer.auth_key, sample_packet, sizeof sample_packet, roc, bad, WA_WARP_TAG_LEN) == 0) { wa_log("srtp selftest: warp accepted forged"); ok |= 1; }

    // ROC trackers: a 0xFFFF->0x0000 wrap bumps once; a reordered dip does not.
    wa_roc_send tx = { 0 };
    wa_roc_send_advance(&tx, 0xFFFE);
    wa_roc_send_advance(&tx, 0xFFFF);
    if (wa_roc_send_advance(&tx, 0x0000) != 1 || wa_roc_send_advance(&tx, 0x0000) != 1) { wa_log("srtp selftest: send ROC"); ok |= 1; }
    wa_roc_recv rx = { 0 };
    wa_roc_recv_commit(&rx, wa_roc_recv_estimate(&rx, 0xFFFE), 0xFFFE);
    wa_roc_recv_commit(&rx, wa_roc_recv_estimate(&rx, 0xFFFF), 0xFFFF);
    uint32_t v = wa_roc_recv_estimate(&rx, 0x0000);
    wa_roc_recv_commit(&rx, v, 0x0000);
    if (v != 1 || rx.roc != 1) { wa_log("srtp selftest: recv ROC"); ok |= 1; }

    return ok ? -1 : 0;
}
