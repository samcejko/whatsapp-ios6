#include "wa_message.h"
#include "wa_proto.h"
#include "wa_crypto.h"

#include <string.h>
#include <stdlib.h>

int wa_msg_text_plaintext(const char *text, wa_buf *out)
{
    return pb_put_string(out, 1, text);   // Message.conversation = 1
}

int wa_msg_ext_text_plaintext(const char *text, const char *quoted_id, const char *quoted_sender,
                              const char *quoted_text, wa_buf *out)
{
    wa_buf ext;
    wa_buf_init(&ext);
    int rc = pb_put_string(&ext, 1, text ? text : "");
    if (rc == 0 && quoted_id && quoted_id[0]) {
        wa_buf ctx, qmsg;
        wa_buf_init(&ctx);
        wa_buf_init(&qmsg);
        rc = pb_put_string(&ctx, 1, quoted_id);   // ContextInfo.stanzaId = 1
        if (rc == 0 && quoted_sender && quoted_sender[0])
            rc = pb_put_string(&ctx, 2, quoted_sender); // ContextInfo.participant = 2
        if (rc == 0) {
            rc = pb_put_string(&qmsg, 1, (quoted_text && quoted_text[0]) ? quoted_text : " ");
            if (rc == 0) rc = pb_put_message(&ctx, 3, &qmsg); // ContextInfo.quotedMessage = 3
        }
        if (rc == 0) rc = pb_put_message(&ext, 17, &ctx); // ExtendedTextMessage.contextInfo = 17
        wa_buf_free(&ctx);
        wa_buf_free(&qmsg);
    }
    if (rc == 0) rc = pb_put_message(out, 6, &ext); // Message.extendedTextMessage = 6
    wa_buf_free(&ext);
    return rc;
}

int wa_msg_reaction_plaintext(const char *target_id, const char *remote_jid, int from_me,
                              const char *emoji, int64_t ts_ms, wa_buf *out)
{
    wa_buf key, react;
    wa_buf_init(&key);
    wa_buf_init(&react);
    int rc = 0;
    if (remote_jid && remote_jid[0]) rc = pb_put_string(&key, 1, remote_jid);
    if (rc == 0) rc = pb_put_varint(&key, 2, from_me ? 1 : 0);
    if (rc == 0 && target_id) rc = pb_put_string(&key, 3, target_id);
    if (rc == 0) rc = pb_put_message(&react, 1, &key); // ReactionMessage.key = 1
    if (rc == 0) rc = pb_put_string(&react, 2, emoji ? emoji : ""); // ReactionMessage.text = 2
    if (rc == 0 && ts_ms > 0) rc = pb_put_varint(&react, 4, (uint64_t)ts_ms); // senderTimestampMs = 4
    if (rc == 0) rc = pb_put_message(out, 46, &react); // Message.reactionMessage = 46
    wa_buf_free(&key);
    wa_buf_free(&react);
    return rc;
}

int wa_msg_dsm_plaintext(const char *destination_jid, const uint8_t *inner_msg, size_t inner_len, wa_buf *out)
{
    wa_buf dsm;
    wa_buf_init(&dsm);
    int rc = pb_put_string(&dsm, 1, destination_jid);
    if (rc == 0) rc = pb_put_bytes(&dsm, 2, inner_msg, inner_len);
    if (rc == 0) rc = pb_put_message(out, 31, &dsm);   // Message.deviceSentMessage = 31
    wa_buf_free(&dsm);
    return rc;
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

static void unwrap_message_containers(const uint8_t **msg, size_t *len)
{
    for (int depth = 0; depth < 4; depth++) {
        const uint8_t *wrap = NULL, *inner = NULL;
        size_t wl = 0, il = 0;
        if (pb_get_bytes(*msg, *len, 31, &wrap, &wl) && pb_get_bytes(wrap, wl, 2, &inner, &il)) {
            *msg = inner; *len = il; continue; // deviceSentMessage (31) -> message (2)
        }
        static const uint32_t fmsg_fields[] = { 40, 37, 55, 53, 58, 74 };
        int unwrapped = 0;
        for (size_t i = 0; i < sizeof(fmsg_fields)/sizeof(fmsg_fields[0]); i++) {
            if (pb_get_bytes(*msg, *len, fmsg_fields[i], &wrap, &wl) && pb_get_bytes(wrap, wl, 1, &inner, &il)) {
                *msg = inner; *len = il; unwrapped = 1; break;
            }
        }
        if (unwrapped) continue;
        // protocolMessage (12) -> editedMessage (14) -> message
        if (pb_get_bytes(*msg, *len, 12, &wrap, &wl) && pb_get_bytes(wrap, wl, 14, &inner, &il)) {
            *msg = inner; *len = il; continue;
        }
        break;
    }
}

char *wa_msg_parse_dsm_dest(const uint8_t *message, size_t len)
{
    const uint8_t *dsm;
    size_t dl;
    if (pb_get_bytes(message, len, 31, &dsm, &dl)) {
        return pb_get_string(dsm, dl, 1); // DeviceSentMessage.destinationJid = 1
    }
    return NULL;
}

char *wa_msg_parse_text(const uint8_t *message, size_t len)
{
    unwrap_message_containers(&message, &len);
    char *s = pb_get_string(message, len, 1);   // conversation
    if (s) return s;
    const uint8_t *ext;
    size_t el;
    if (pb_get_bytes(message, len, 6, &ext, &el))   // extendedTextMessage
        return pb_get_string(ext, el, 1);           // ExtendedTextMessage.text = 1
    return NULL;
}

int wa_msg_parse_quoted(const uint8_t *message, size_t len, char *quoted_id_out, size_t id_cap,
                        char *quoted_text_out, size_t text_cap)
{
    if (quoted_id_out && id_cap) quoted_id_out[0] = 0;
    if (quoted_text_out && text_cap) quoted_text_out[0] = 0;
    unwrap_message_containers(&message, &len);
    const uint8_t *sub = NULL, *ctx = NULL;
    size_t sl = 0, cl = 0;
    static const uint32_t sub_fields[] = { 6, 3, 9, 7, 8 }; // extText, image, video, doc, audio
    for (size_t i = 0; i < sizeof(sub_fields)/sizeof(sub_fields[0]); i++) {
        if (pb_get_bytes(message, len, sub_fields[i], &sub, &sl) && pb_get_bytes(sub, sl, 17, &ctx, &cl)) {
            char *sid = pb_get_string(ctx, cl, 1); // ContextInfo.stanzaId = 1
            if (!sid || !sid[0]) { free(sid); return 0; }
            if (quoted_id_out && id_cap) snprintf(quoted_id_out, id_cap, "%s", sid);
            free(sid);
            const uint8_t *qm = NULL;
            size_t qml = 0;
            if (pb_get_bytes(ctx, cl, 3, &qm, &qml)) { // ContextInfo.quotedMessage = 3
                char *qt = wa_msg_parse_text(qm, qml);
                if (qt) {
                    if (quoted_text_out && text_cap) snprintf(quoted_text_out, text_cap, "%s", qt);
                    free(qt);
                }
            }
            return 1;
        }
    }
    return 0;
}

int wa_msg_parse_reaction(const uint8_t *message, size_t len, char *target_id_out, size_t id_cap,
                          char *emoji_out, size_t emoji_cap)
{
    if (target_id_out && id_cap) target_id_out[0] = 0;
    if (emoji_out && emoji_cap) emoji_out[0] = 0;
    unwrap_message_containers(&message, &len);
    const uint8_t *react = NULL, *key = NULL;
    size_t rl = 0, kl = 0;
    if (!pb_get_bytes(message, len, 46, &react, &rl)) return 0;
    if (!pb_get_bytes(react, rl, 1, &key, &kl)) return 0;
    char *tid = pb_get_string(key, kl, 3); // MessageKey.id = 3
    if (!tid || !tid[0]) { free(tid); return 0; }
    if (target_id_out && id_cap) snprintf(target_id_out, id_cap, "%s", tid);
    free(tid);
    char *em = pb_get_string(react, rl, 2); // ReactionMessage.text = 2
    if (em) {
        if (emoji_out && emoji_cap) snprintf(emoji_out, emoji_cap, "%s", em);
        free(em);
    }
    return 1;
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
    wa_node_set_bytes(e, enc, enc_len);
    return m;
}

wa_node *wa_msg_build_dm(const char *to_user, const char *id, const wa_msg_devenc *devs, int ndevs,
                         const uint8_t *device_identity, size_t di_len, const char *peer_recipient_pn)
{
    wa_node *m = wa_node_new("message");
    wa_node_attr(m, "to", to_user);
    wa_node_attr(m, "id", id);
    wa_node_attr(m, "type", "text");
    if (peer_recipient_pn && peer_recipient_pn[0]) {
        wa_node_attr(m, "peer_recipient_pn", peer_recipient_pn);
    }
    wa_node *parts = wa_node_add_new(m, "participants");
    int any_pkmsg = 0;
    for (int i = 0; i < ndevs; i++) {
        wa_node *to = wa_node_add_new(parts, "to");
        wa_node_attr(to, "jid", devs[i].jid);
        wa_node *e = wa_node_add_new(to, "enc");
        wa_node_attr(e, "v", "2");
        wa_node_attr(e, "type", devs[i].enc_type);
        wa_node_set_bytes(e, devs[i].enc, devs[i].enc_len);
        if (strcmp(devs[i].enc_type, "pkmsg") == 0) any_pkmsg = 1;
    }
    if (any_pkmsg && device_identity && di_len) wa_node_set_bytes(wa_node_add_new(m, "device-identity"), device_identity, di_len);
    return m;
}

int wa_msg_parse_incoming(const wa_node *message, wa_msg_incoming *out)
{
    if (!wa_node_tag_is(message, "message")) return 0;
    const wa_node *enc = wa_node_find(message, "enc");
    if (!enc || !enc->has_content) return 0;
    memset(out, 0, sizeof *out);
    out->from = wa_node_get(message, "from");
    out->id = wa_node_get(message, "id");
    out->type = wa_node_get(message, "type");
    out->notify = wa_node_get(message, "notify");
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
    wa_node *dm = wa_msg_build_dm("99887766@lid", "MID1", devs, 2, di, sizeof di, "420777123456@s.whatsapp.net");
    wa_buf db;
    wa_buf_init(&db);
    ok = wa_node_encode(dm, &db) == 0;
    wa_node *dmb = ok ? wa_node_decode(db.data + 1, db.len - 1) : NULL;
    if (dmb) {
        wa_node *parts = wa_node_child(dmb, "participants");
        wa_node *di_node = wa_node_child(dmb, "device-identity");
        ok = parts && parts->nchildren == 2 && di_node && di_node->content_len == 64 &&
             wa_node_find(parts, "enc") != NULL &&
             strcmp(wa_node_get_or(dmb, "peer_recipient_pn", ""), "420777123456@s.whatsapp.net") == 0;
    } else ok = 0;
    wa_node_free(dm);
    wa_node_free(dmb);
    wa_buf_free(&db);
    if (!ok) { wa_log("message selftest: DM fanout failed"); return -1; }

    // --- DeviceSentMessage plaintext test ---
    wa_buf inner, dsm_pt;
    wa_buf_init(&inner);
    wa_buf_init(&dsm_pt);
    ok = (wa_msg_text_plaintext("test dsm", &inner) == 0 &&
          wa_msg_dsm_plaintext("10561408512047@lid", inner.data, inner.len, &dsm_pt) == 0 &&
          dsm_pt.len > 0);
    const uint8_t *dsm_bytes = NULL;
    size_t dsm_len = 0;
    ok = ok && pb_get_bytes(dsm_pt.data, dsm_pt.len, 31, &dsm_bytes, &dsm_len);
    char *dest = ok ? pb_get_string(dsm_bytes, dsm_len, 1) : NULL;
    ok = ok && dest && strcmp(dest, "10561408512047@lid") == 0;
    free(dest);
    wa_buf_free(&inner);
    wa_buf_free(&dsm_pt);
    if (!ok) { wa_log("message selftest: DSM plaintext failed"); return -1; }
    return 0;
}
