#include "wa_message.h"
#include "wa_proto.h"
#include "wa_crypto.h"

#include <string.h>
#include <stdlib.h>

int wa_msg_text_plaintext(const char *text, wa_buf *out)
{
    return pb_put_string(out, 1, text);   // Message.conversation = 1
}

int wa_msg_callkey_plaintext(const uint8_t call_key[32], wa_buf *out)
{
    wa_buf call;
    wa_buf_init(&call);
    int rc = pb_put_bytes(&call, 1, call_key, 32);   // Call.callKey = 1
    if (rc == 0) rc = pb_put_message(out, 10, &call);  // Message.call = 10
    wa_buf_free(&call);
    return rc;
}

int wa_msg_encrypt(wa_sig_session *s, const wa_sig_keypair *our_identity, const uint8_t *plaintext, size_t len,
                   int *is_prekey, wa_buf *enc_out)
{
    wa_buf padded;
    wa_buf_init(&padded);
    int rc = wa_sig_pad(plaintext, len, &padded);
    if (rc == 0) rc = wa_sig_encrypt(s, our_identity, padded.data, padded.len, is_prekey, enc_out);
    wa_buf_free(&padded);
    return rc;
}

int wa_msg_decrypt(wa_sig_session *s, const wa_sig_keypair *our_identity, const wa_sig_keypair *our_signed_prekey,
                   const wa_sig_keypair *our_one_time, int is_prekey, const uint8_t *enc, size_t len, wa_buf *out)
{
    wa_buf padded;
    wa_buf_init(&padded);
    int rc = is_prekey ? wa_sig_decrypt_prekey(s, our_identity, our_signed_prekey, our_one_time, enc, len, &padded)
                       : wa_sig_decrypt(s, enc, len, &padded);
    if (rc == 0) rc = wa_sig_unpad(padded.data, padded.len, out);
    wa_buf_free(&padded);
    return rc;
}

char *wa_msg_parse_text(const uint8_t *message, size_t len)
{
    char *s = pb_get_string(message, len, 1);   // conversation
    if (s) return s;
    const uint8_t *ext;
    size_t el;
    if (pb_get_bytes(message, len, 6, &ext, &el))   // extendedTextMessage
        return pb_get_string(ext, el, 1);           // ExtendedTextMessage.text = 1
    return NULL;
}

wa_node *wa_msg_build_node(const char *to, const char *id, const uint8_t *enc, size_t enc_len, const char *enc_type)
{
    wa_node *m = wa_node_new("message");
    wa_node_attr(m, "to", to);
    wa_node_attr(m, "id", id);
    wa_node_attr(m, "type", "text");
    wa_node *e = wa_node_add_new(m, "enc");
    wa_node_attr(e, "v", "2");
    wa_node_attr(e, "type", enc_type);
    wa_node_attr(e, "count", "0");
    wa_node_set_bytes(e, enc, enc_len);
    return m;
}

wa_node *wa_msg_build_dm(const char *to_user, const char *id, const wa_msg_devenc *devs, int ndevs,
                         const uint8_t *device_identity, size_t di_len)
{
    wa_node *m = wa_node_new("message");
    wa_node_attr(m, "to", to_user);
    wa_node_attr(m, "id", id);
    wa_node_attr(m, "type", "text");
    wa_node *parts = wa_node_add_new(m, "participants");
    int any_pkmsg = 0;
    for (int i = 0; i < ndevs; i++) {
        wa_node *to = wa_node_add_new(parts, "to");
        wa_node_attr(to, "jid", devs[i].jid);
        wa_node *e = wa_node_add_new(to, "enc");
        wa_node_attr(e, "v", "2");
        wa_node_attr(e, "type", devs[i].enc_type);
        wa_node_attr(e, "count", "0");
        wa_node_set_bytes(e, devs[i].enc, devs[i].enc_len);
        if (strcmp(devs[i].enc_type, "pkmsg") == 0) any_pkmsg = 1;
    }
    if (any_pkmsg && device_identity && di_len) wa_node_set_bytes(wa_node_add_new(m, "device-identity"), device_identity, di_len);
    return m;
}

int wa_msg_parse_incoming(const wa_node *message, wa_msg_incoming *out)
{
    if (!wa_node_tag_is(message, "message")) return 0;
    wa_node *enc = wa_node_child(message, "enc");
    if (!enc || !enc->has_content) return 0;
    memset(out, 0, sizeof *out);
    out->from = wa_node_get(message, "from");
    out->id = wa_node_get(message, "id");
    out->type = wa_node_get(message, "type");
    out->enc_type = wa_node_get_or(enc, "type", "msg");
    out->enc = enc->content;
    out->enc_len = enc->content_len;
    return 1;
}

// -------------------------------------------------------------------------------------------------- self test

int wa_message_selftest(void)
{
    // Alice and Bob establish a Signal session, Alice sends a text and a call-key, Bob decrypts and parses both.
    wa_sig_keypair alice_id, bob_id, bob_spk, bob_otp;
    if (wa_curve_keypair(alice_id.priv, alice_id.pub) || wa_curve_keypair(bob_id.priv, bob_id.pub) ||
        wa_curve_keypair(bob_spk.priv, bob_spk.pub) || wa_curve_keypair(bob_otp.priv, bob_otp.pub))
        return -1;
    wa_sig_bundle b;
    memset(&b, 0, sizeof b);
    memcpy(b.identity_pub, bob_id.pub, 32);
    b.signed_prekey_id = 1;
    memcpy(b.signed_prekey_pub, bob_spk.pub, 32);
    uint8_t spk33[33];
    spk33[0] = 5;
    memcpy(spk33 + 1, bob_spk.pub, 32);
    if (wa_curve_sign(b.signed_prekey_sig, bob_id.priv, spk33, 33) != 0) return -1;
    b.has_one_time = 1;
    b.prekey_id = 9;
    memcpy(b.one_time_pub, bob_otp.pub, 32);

    wa_sig_session alice, bob;
    memset(&bob, 0, sizeof bob);
    if (wa_sig_init_outgoing(&alice, &alice_id, 77, &b) != 0) return -1;

    // --- text ---
    wa_buf pt, enc, got, node_buf;
    wa_buf_init(&pt);
    wa_buf_init(&enc);
    wa_buf_init(&got);
    wa_buf_init(&node_buf);
    int is_prekey = 0, ok = 0;
    if (wa_msg_text_plaintext("ahoj cau :)", &pt) == 0 &&
        wa_msg_encrypt(&alice, &alice_id, pt.data, pt.len, &is_prekey, &enc) == 0 && is_prekey == 1) {
        wa_node *node = wa_msg_build_node("420777@s.whatsapp.net", "ABCD", enc.data, enc.len, is_prekey ? "pkmsg" : "msg");
        ok = wa_node_encode(node, &node_buf) == 0;
        wa_node *back = ok ? wa_node_decode(node_buf.data + 1, node_buf.len - 1) : NULL;
        wa_msg_incoming in;
        if (back && wa_msg_parse_incoming(back, &in)) {
            wa_buf msg;
            wa_buf_init(&msg);
            if (wa_msg_decrypt(&bob, &bob_id, &bob_spk, &bob_otp, strcmp(in.enc_type, "pkmsg") == 0,
                               in.enc, in.enc_len, &msg) == 0) {
                char *text = wa_msg_parse_text(msg.data, msg.len);
                ok = text && strcmp(text, "ahoj cau :)") == 0;
                free(text);
            } else ok = 0;
            wa_buf_free(&msg);
        } else ok = 0;
        wa_node_free(node);
        wa_node_free(back);
    } else ok = 0;
    wa_buf_free(&pt);
    wa_buf_free(&enc);
    wa_buf_free(&got);
    wa_buf_free(&node_buf);
    if (!ok) { wa_log("message selftest: text round trip failed"); return -1; }

    // --- call key ---
    uint8_t call_key[32];
    for (int i = 0; i < 32; i++) call_key[i] = (uint8_t)(i * 3 + 1);
    wa_buf ck, cenc, cmsg;
    wa_buf_init(&ck);
    wa_buf_init(&cenc);
    wa_buf_init(&cmsg);
    int ip = 0;
    ok = 0;
    if (wa_msg_callkey_plaintext(call_key, &ck) == 0 &&
        wa_msg_encrypt(&alice, &alice_id, ck.data, ck.len, &ip, &cenc) == 0 &&
        wa_msg_decrypt(&bob, &bob_id, &bob_spk, &bob_otp, ip, cenc.data, cenc.len, &cmsg) == 0) {
        const uint8_t *call, *key;
        size_t cl, kl;
        ok = pb_get_bytes(cmsg.data, cmsg.len, 10, &call, &cl) &&
             pb_get_bytes(call, cl, 1, &key, &kl) && kl == 32 && memcmp(key, call_key, 32) == 0;
    }
    wa_buf_free(&ck);
    wa_buf_free(&cenc);
    wa_buf_free(&cmsg);
    if (!ok) { wa_log("message selftest: call-key round trip failed"); return -1; }

    // --- DM fanout node (two devices, one pkmsg -> device-identity attached) ---
    uint8_t ct0[20], ct1[22], di[64];
    for (int i = 0; i < 20; i++) ct0[i] = (uint8_t)i;
    for (int i = 0; i < 22; i++) ct1[i] = (uint8_t)(i + 5);
    for (int i = 0; i < 64; i++) di[i] = (uint8_t)(i + 1);
    wa_msg_devenc devs[2] = {
        { "420777123456:0@s.whatsapp.net", ct0, sizeof ct0, "pkmsg" },
        { "420777123456:1@s.whatsapp.net", ct1, sizeof ct1, "msg" },
    };
    wa_node *dm = wa_msg_build_dm("420777123456@s.whatsapp.net", "MID1", devs, 2, di, sizeof di);
    wa_buf db;
    wa_buf_init(&db);
    ok = wa_node_encode(dm, &db) == 0;
    wa_node *dmb = ok ? wa_node_decode(db.data + 1, db.len - 1) : NULL;
    if (dmb) {
        wa_node *parts = wa_node_child(dmb, "participants");
        wa_node *di_node = wa_node_child(dmb, "device-identity");
        ok = parts && parts->nchildren == 2 && di_node && di_node->content_len == 64 &&
             wa_node_find(parts, "enc") != NULL;
    } else ok = 0;
    wa_node_free(dm);
    wa_node_free(dmb);
    wa_buf_free(&db);
    if (!ok) { wa_log("message selftest: DM fanout failed"); return -1; }
    return 0;
}
