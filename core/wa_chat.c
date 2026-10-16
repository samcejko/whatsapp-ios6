#include "wa_chat.h"
#include "wa_user.h"
#include "wa_message.h"
#include "wa_signal.h"
#include "wa_store.h"
#include "wa_node.h"
#include "wa_util.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void our_identity(wa_client *c, wa_sig_keypair *out)
{
    wa_device *d = wa_client_device(c);
    memcpy(out->priv, d->identity_key.priv, 32);
    memcpy(out->pub, d->identity_key.pub, 32);
}

static void our_signed_prekey(wa_client *c, wa_sig_keypair *out)
{
    wa_device *d = wa_client_device(c);
    memcpy(out->priv, d->signed_pre_key.priv, 32);
    memcpy(out->pub, d->signed_pre_key.pub, 32);
}

// Find the <user jid=..> node in a prekey-fetch result and parse its bundle.
static int fetch_bundle(wa_client *c, const char *device_jid, wa_sig_bundle *out)
{
    const char *jids[1] = { device_jid };
    wa_node *iq = wa_user_build_prekey_iq(jids, 1);
    wa_node *res = wa_client_request_sync(c, iq, 15000);
    wa_node_free(iq);
    if (!res) return -1;
    int rc = -1;
    wa_node *list = wa_node_find(res, "list");
    if (list) {
        for (int i = 0; i < list->nchildren; i++) {
            wa_node *u = list->children[i];
            if (wa_node_tag_is(u, "user") && wa_user_parse_bundle(u, out) == 0) { rc = 0; break; }
        }
    }
    wa_node_free(res);
    return rc;
}

// Ensure a Signal session exists for device_jid (fetch a bundle and init one if not). 0 or -1.
static int ensure_session(wa_client *c, wa_sess_store *sessions, const char *device_jid)
{
    if (wa_sess_has(sessions, device_jid)) return 0;
    wa_sig_bundle b;
    if (fetch_bundle(c, device_jid, &b) != 0) return -1;
    wa_sig_keypair id;
    our_identity(c, &id);
    wa_sig_session sess;
    if (wa_sig_init_outgoing(&sess, &id, wa_client_device(c)->registration_id, &b) != 0) return -1;
    return wa_sess_put(sessions, device_jid, &sess);
}

// Resolve a bare user's device JIDs via usync.
static int resolve_devices(wa_client *c, const char *user_jid, wa_device_list *out)
{
    char sid[24];
    wa_client_next_id(c, sid, sizeof sid);
    const char *users[1] = { user_jid };
    wa_node *iq = wa_user_build_devices_iq(users, 1, sid);
    wa_node *res = wa_client_request_sync(c, iq, 15000);
    wa_node_free(iq);
    if (!res) return -1;
    int rc = wa_user_parse_devices(res, out);
    wa_node_free(res);
    return rc;
}

int wa_chat_send_text(wa_client *c, wa_sess_store *sessions, const char *to_user_jid, const char *text)
{
    wa_jid tj;
    if (wa_jid_parse(to_user_jid, &tj) != 0) return -1;

    // devices of the recipient, and our own other devices (so our other linked devices see the message too)
    wa_device_list recip, mine;
    if (resolve_devices(c, to_user_jid, &recip) != 0) return -1;
    wa_jid me;
    int have_me = wa_jid_parse(wa_client_device(c)->pn, &me) == 0;
    char my_user[80];
    if (have_me) { snprintf(my_user, sizeof my_user, "%s@%s", me.user, me.server); if (resolve_devices(c, my_user, &mine) != 0) mine.ndevices = 0; }
    else mine.ndevices = 0;

    wa_sig_keypair id;
    our_identity(c, &id);

    // encrypt the Message plaintext once per device
    wa_buf plaintext;
    wa_buf_init(&plaintext);
    if (wa_msg_text_plaintext(text, &plaintext) != 0) { wa_buf_free(&plaintext); return -1; }

    wa_msg_devenc devs[WA_MAX_DEVICES * 2];
    wa_buf encbufs[WA_MAX_DEVICES * 2];
    char typestr[WA_MAX_DEVICES * 2][8];
    int ndevs = 0, any_pkmsg = 0, reached_recipient = 0;

    wa_device_list *lists[2] = { &recip, &mine };
    for (int li = 0; li < 2; li++) {
        for (int i = 0; i < lists[li]->ndevices && ndevs < (int)(sizeof devs / sizeof devs[0]); i++) {
            const char *djid = lists[li]->jids[i];
            if (have_me && wa_jid_same_user(djid, wa_client_device(c)->pn)) {
                wa_jid dj;
                if (wa_jid_parse(djid, &dj) == 0 && dj.device == me.device) continue;   // skip ourselves
            }
            if (ensure_session(c, sessions, djid) != 0) continue;
            wa_sig_session *s = wa_sess_get(sessions, djid);
            wa_buf_init(&encbufs[ndevs]);
            int is_prekey = 0;
            if (wa_msg_encrypt(s, &id, plaintext.data, plaintext.len, &is_prekey, &encbufs[ndevs]) != 0) { wa_buf_free(&encbufs[ndevs]); continue; }
            snprintf(typestr[ndevs], 8, "%s", is_prekey ? "pkmsg" : "msg");
            devs[ndevs].jid = djid;
            devs[ndevs].enc = encbufs[ndevs].data;
            devs[ndevs].enc_len = encbufs[ndevs].len;
            devs[ndevs].enc_type = typestr[ndevs];
            if (is_prekey) any_pkmsg = 1;
            if (li == 0) reached_recipient = 1;
            ndevs++;
        }
    }
    wa_buf_free(&plaintext);

    int rc = -1;
    if (reached_recipient) {
        char id_str[24];
        wa_client_next_id(c, id_str, sizeof id_str);
        wa_device *d = wa_client_device(c);
        wa_node *msg = wa_msg_build_dm(to_user_jid, id_str, devs, ndevs,
                                       any_pkmsg ? d->account : NULL, any_pkmsg ? d->account_len : 0);
        rc = wa_client_send(c, msg);
        wa_node_free(msg);
    }
    for (int i = 0; i < ndevs; i++) wa_buf_free(&encbufs[i]);
    return rc;
}

char *wa_chat_handle_message(wa_client *c, wa_sess_store *sessions, const wa_node *message, char *frombuf, size_t fromcap)
{
    wa_msg_incoming in;
    if (!wa_msg_parse_incoming(message, &in) || !in.from) return NULL;
    wa_jid_to_non_ad(in.from, frombuf, fromcap);

    wa_sig_keypair id, spk;
    our_identity(c, &id);
    our_signed_prekey(c, &spk);

    int is_prekey = strcmp(in.enc_type, "pkmsg") == 0;

    // A PreKeySignalMessage was encrypted against one of OUR one-time prekeys; X3DH needs that private key.
    // Pull the id out of the header and look it up in the device pool (passing NULL would drop the DH4 term).
    wa_sig_keypair otp;
    const wa_sig_keypair *otp_ptr = NULL;
    if (is_prekey) {
        uint32_t reg_id = 0, prekey_id = 0, spk_id = 0;
        int has_pk = 0;
        uint8_t base_pub[32], id_pub[32];
        if (wa_sig_prekey_header(in.enc, in.enc_len, &reg_id, &has_pk, &prekey_id, &spk_id, base_pub, id_pub) == 0 && has_pk) {
            const wa_keypair *k = wa_device_find_prekey(wa_client_device(c), prekey_id);
            if (k) { memcpy(otp.priv, k->priv, 32); memcpy(otp.pub, k->pub, 32); otp_ptr = &otp; }
            else wa_log("chat: pkmsg uses one-time prekey id %u we do not hold", (unsigned)prekey_id);
        }
    }

    wa_sig_session fresh;
    wa_sig_session *s = wa_sess_get(sessions, in.from);
    wa_sig_session *use = s;
    if (is_prekey || !use) { use = &fresh; if (s) fresh = *s; else memset(&fresh, 0, sizeof fresh); }

    wa_buf msg;
    wa_buf_init(&msg);
    int rc = wa_msg_decrypt(use, &id, &spk, otp_ptr, is_prekey, in.enc, in.enc_len, &msg);
    if (rc == 0) wa_sess_put(sessions, in.from, use);   // persist the advanced/established session
    char *text = rc == 0 ? wa_msg_parse_text(msg.data, msg.len) : NULL;
    wa_buf_free(&msg);
    return text;
}

int wa_chat_encrypt_callkey(wa_client *c, wa_sess_store *sessions, const char *device_jid,
                            const uint8_t call_key[32], int *is_prekey, wa_buf *out)
{
    if (ensure_session(c, sessions, device_jid) != 0) return -1;
    wa_sig_session *s = wa_sess_get(sessions, device_jid);
    wa_sig_keypair id;
    our_identity(c, &id);
    wa_buf pt;
    wa_buf_init(&pt);
    int rc = wa_msg_callkey_plaintext(call_key, &pt);
    if (rc == 0) rc = wa_msg_encrypt(s, &id, pt.data, pt.len, is_prekey, out);
    wa_buf_free(&pt);
    return rc;
}
