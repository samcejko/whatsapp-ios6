#include "wa_chat.h"
#include "wa_user.h"
#include "wa_message.h"
#include "wa_signal.h"
#include "wa_store.h"
#include "wa_node.h"
#include "wa_util.h"
#include "wa_history.h"

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

int wa_chat_send_text_with_id(wa_client *c, wa_sess_store *sessions, const char *to_user_jid, const char *text,
                             const char *msg_id, char *out_msg_id, size_t out_msg_id_cap)
{
    wa_jid tj;
    if (wa_jid_parse(to_user_jid, &tj) != 0) return -1;

    // devices of the recipient, and our own other devices (so our other linked devices see the message too)
    wa_device_list recip, mine;
    memset(&recip, 0, sizeof recip);
    memset(&mine, 0, sizeof mine);
    if (resolve_devices(c, to_user_jid, &recip) != 0) return -1;

    // Modern WhatsApp requires addressing via LID if available
    const char *dest_user = to_user_jid;
    const char *peer_recipient_pn = NULL;
    char recip_lid_user[80];
    if (recip.lid[0] && strstr(to_user_jid, "@s.whatsapp.net")) {
        dest_user = recip.lid;
        peer_recipient_pn = to_user_jid;
        wa_jid rlj;
        if (wa_jid_parse(recip.lid, &rlj) == 0) {
            snprintf(recip_lid_user, sizeof recip_lid_user, "%s", rlj.user);
            for (int i = 0; i < recip.ndevices; i++) {
                wa_jid dj;
                if (wa_jid_parse(recip.jids[i], &dj) == 0) {
                    wa_jid_format(recip.jids[i], sizeof recip.jids[0], recip_lid_user, dj.device, "lid");
                }
            }
        }
    }

    wa_device *d = wa_client_device(c);
    wa_jid me_pn, me_lid;
    int have_me_pn = wa_jid_parse(d->pn, &me_pn) == 0;
    int have_me_lid = wa_jid_parse(d->lid, &me_lid) == 0;
    int my_dev_id = have_me_lid ? me_lid.device : (have_me_pn ? me_pn.device : 0);

    char my_user[80];
    if (have_me_lid) {
        snprintf(my_user, sizeof my_user, "%s@%s", me_lid.user, me_lid.server);
        if (resolve_devices(c, my_user, &mine) != 0) mine.ndevices = 0;
    } else if (have_me_pn) {
        snprintf(my_user, sizeof my_user, "%s@%s", me_pn.user, me_pn.server);
        if (resolve_devices(c, my_user, &mine) != 0) mine.ndevices = 0;
    }

    wa_sig_keypair id;
    our_identity(c, &id);

    // Prepare plaintexts: recipient receives raw Message (field 1: conversation),
    // own companion/phone devices receive DeviceSentMessage (field 31)
    wa_buf pt_recip, pt_mine;
    wa_buf_init(&pt_recip);
    wa_buf_init(&pt_mine);
    if (wa_msg_text_plaintext(text, &pt_recip) != 0) { wa_buf_free(&pt_recip); return -1; }
    if (wa_msg_dsm_plaintext(dest_user, pt_recip.data, pt_recip.len, &pt_mine) != 0) {
        wa_buf_free(&pt_recip);
        wa_buf_free(&pt_mine);
        return -1;
    }

    wa_msg_devenc devs[WA_MAX_DEVICES * 2];
    wa_buf encbufs[WA_MAX_DEVICES * 2];
    char typestr[WA_MAX_DEVICES * 2][8];
    int ndevs = 0, any_pkmsg = 0, reached_recipient = 0;

    wa_device_list *lists[2] = { &recip, &mine };
    for (int li = 0; li < 2; li++) {
        const uint8_t *plain = (li == 0) ? pt_recip.data : pt_mine.data;
        size_t plain_len = (li == 0) ? pt_recip.len : pt_mine.len;

        for (int i = 0; i < lists[li]->ndevices && ndevs < (int)(sizeof devs / sizeof devs[0]); i++) {
            const char *djid = lists[li]->jids[i];
            wa_jid dj;
            if (wa_jid_parse(djid, &dj) == 0) {
                int is_own_user = (have_me_pn && strcmp(dj.user, me_pn.user) == 0) ||
                                  (have_me_lid && strcmp(dj.user, me_lid.user) == 0);
                if (is_own_user && dj.device == my_dev_id) continue;   // skip ourselves
            }
            if (ensure_session(c, sessions, djid) != 0) continue;
            wa_sig_session *s = wa_sess_get(sessions, djid);
            wa_buf_init(&encbufs[ndevs]);
            int is_prekey = 0;
            if (wa_msg_encrypt(s, &id, plain, plain_len, &is_prekey, &encbufs[ndevs]) != 0) { wa_buf_free(&encbufs[ndevs]); continue; }
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
    wa_buf_free(&pt_recip);
    wa_buf_free(&pt_mine);

    int rc = -1;
    if (reached_recipient) {
        char id_str[40];
        if (msg_id && msg_id[0]) snprintf(id_str, sizeof id_str, "%s", msg_id);
        else wa_client_next_id(c, id_str, sizeof id_str);
        if (out_msg_id && out_msg_id_cap) snprintf(out_msg_id, out_msg_id_cap, "%s", id_str);
        wa_node *msg = wa_msg_build_dm(dest_user, id_str, devs, ndevs,
                                       any_pkmsg ? d->account : NULL, any_pkmsg ? d->account_len : 0,
                                       peer_recipient_pn);
        rc = wa_client_send(c, msg);
        wa_node_free(msg);
    }
    for (int i = 0; i < ndevs; i++) wa_buf_free(&encbufs[i]);
    return rc;
}

int wa_chat_send_text(wa_client *c, wa_sess_store *sessions, const char *to_user_jid, const char *text)
{
    return wa_chat_send_text_with_id(c, sessions, to_user_jid, text, NULL, NULL, 0);
}

// Acknowledge a delivered history-sync chunk: <receipt to="<own pn>" id="<msg id>" type="hist_sync"/>. Without this
// the phone keeps re-uploading the chunk. (Confirmed vs whatsapp-rust send_protocol_receipt / ReceiptType::HistorySync.)
static void send_hist_sync_receipt(wa_client *c, const char *msg_id)
{
    if (!msg_id) return;
    const char *pn = wa_client_device(c)->pn;
    if (!pn[0]) return;
    char own[80];
    wa_jid_to_non_ad(pn, own, sizeof own);
    wa_node *r = wa_node_new("receipt");
    wa_node_attr(r, "to", own);
    wa_node_attr(r, "id", msg_id);
    wa_node_attr(r, "type", "hist_sync");
    wa_client_send(c, r);
    wa_node_free(r);
}

char *wa_chat_handle_message(wa_client *c, wa_sess_store *sessions, const wa_node *message, char *frombuf, size_t fromcap,
                             wa_history_result *hist, int *is_history)
{
    if (hist) memset(hist, 0, sizeof *hist);
    if (is_history) *is_history = 0;
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

    // A linked device's initial history arrives as a peer protocolMessage/historySyncNotification. Hand the decrypted
    // Message to the history module; if it is one, acknowledge the chunk and surface its conversations (not text).
    if (rc == 0 && hist) {
        int h = wa_history_from_message(msg.data, msg.len, NULL /* CDN TLS unverified; blob HMAC authenticates it */, hist);
        if (h != 0) {
            send_hist_sync_receipt(c, in.id);
            if (is_history) *is_history = (h == 1);
            wa_buf_free(&msg);
            return NULL;
        }
    }

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
