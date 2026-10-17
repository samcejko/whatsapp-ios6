#include "wa_chat.h"
#include "wa_user.h"
#include "wa_message.h"
#include "wa_signal.h"
#include "wa_store.h"
#include "wa_node.h"
#include "wa_util.h"
#include "wa_history.h"
#include "wa_net.h"
#include "wa_crypto.h"
#include "wa_proto.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

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

int wa_chat_send_message_payload(wa_client *c, wa_sess_store *sessions, const char *to_user_jid,
                                 const uint8_t *msg_bytes, size_t msg_len,
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

    // Prepare plaintexts: recipient receives raw Message,
    // own companion/phone devices receive DeviceSentMessage (field 31)
    wa_buf pt_mine;
    wa_buf_init(&pt_mine);
    if (wa_msg_dsm_plaintext(dest_user, msg_bytes, msg_len, &pt_mine) != 0) {
        return -1;
    }

    wa_msg_devenc devs[WA_MAX_DEVICES * 2];
    wa_buf encbufs[WA_MAX_DEVICES * 2];
    char typestr[WA_MAX_DEVICES * 2][8];
    int ndevs = 0, any_pkmsg = 0, reached_recipient = 0;

    wa_device_list *lists[2] = { &recip, &mine };
    for (int li = 0; li < 2; li++) {
        const uint8_t *plain = (li == 0) ? msg_bytes : pt_mine.data;
        size_t plain_len = (li == 0) ? msg_len : pt_mine.len;

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

int wa_chat_send_text_with_id(wa_client *c, wa_sess_store *sessions, const char *to_user_jid, const char *text,
                             const char *msg_id, char *out_msg_id, size_t out_msg_id_cap)
{
    wa_buf pt;
    wa_buf_init(&pt);
    if (wa_msg_text_plaintext(text, &pt) != 0) { wa_buf_free(&pt); return -1; }
    int rc = wa_chat_send_message_payload(c, sessions, to_user_jid, pt.data, pt.len, msg_id, out_msg_id, out_msg_id_cap);
    wa_buf_free(&pt);
    return rc;
}

int wa_chat_send_ext_text(wa_client *c, wa_sess_store *sessions, const char *to_user_jid, const char *text,
                          const char *quoted_id, const char *quoted_sender, const char *quoted_text,
                          const char *msg_id, char *out_msg_id, size_t out_msg_id_cap)
{
    wa_buf pt;
    wa_buf_init(&pt);
    if (wa_msg_ext_text_plaintext(text, quoted_id, quoted_sender, quoted_text, &pt) != 0) {
        wa_buf_free(&pt);
        return -1;
    }
    int rc = wa_chat_send_message_payload(c, sessions, to_user_jid, pt.data, pt.len, msg_id, out_msg_id, out_msg_id_cap);
    wa_buf_free(&pt);
    return rc;
}

int wa_chat_send_reaction(wa_client *c, wa_sess_store *sessions, const char *to_user_jid,
                          const char *target_msg_id, int target_from_me, const char *emoji)
{
    if (!c || !sessions || !to_user_jid || !target_msg_id) return -1;
    wa_buf pt;
    wa_buf_init(&pt);
    int64_t ts_ms = (int64_t)time(NULL) * 1000LL;
    if (wa_msg_reaction_plaintext(target_msg_id, to_user_jid, target_from_me, emoji, ts_ms, &pt) != 0) {
        wa_buf_free(&pt);
        return -1;
    }
    int rc = wa_chat_send_message_payload(c, sessions, to_user_jid, pt.data, pt.len, NULL, NULL, 0);
    wa_buf_free(&pt);
    return rc;
}

int wa_chat_send_text(wa_client *c, wa_sess_store *sessions, const char *to_user_jid, const char *text)
{
    return wa_chat_send_text_with_id(c, sessions, to_user_jid, text, NULL, NULL, 0);
}

static int upload_media_blob(wa_client *c, const uint8_t *raw, size_t raw_len,
                             const char *hkdf_info, const char *mms_type,
                             uint8_t media_key_out[32], uint8_t file_sha_out[32], uint8_t file_enc_sha_out[32],
                             char *direct_path_out, size_t dp_cap, char *media_url_out, size_t url_cap)
{
    wa_node *iq = wa_node_new("iq");
    wa_node_attr(iq, "to", "@s.whatsapp.net");
    wa_node_attr(iq, "type", "set");
    wa_node_attr(iq, "xmlns", "w:m");
    wa_node_add_new(iq, "media_conn");
    wa_node *res = wa_client_request_sync(c, iq, 10000);
    wa_node_free(iq);
    if (!res) {
        wa_log("chat: media_conn request timed out");
        return -1;
    }
    const wa_node *mc = wa_node_find(res, "media_conn");
    const char *auth = mc ? wa_node_get(mc, "auth") : NULL;
    char hostname[128] = "mmg.whatsapp.net";
    if (mc) {
        const wa_node *hn = wa_node_find(mc, "host");
        if (hn) {
            const char *h = wa_node_get(hn, "hostname");
            if (h && h[0]) snprintf(hostname, sizeof hostname, "%s", h);
        }
    }
    if (!auth || !auth[0]) {
        wa_log("chat: media_conn missing auth");
        wa_node_free(res);
        return -1;
    }
    char auth_copy[512];
    snprintf(auth_copy, sizeof auth_copy, "%s", auth);
    wa_node_free(res);

    wa_random(media_key_out, 32);

    uint8_t exp[112];
    if (wa_hkdf_sha256(NULL, 0, media_key_out, 32, hkdf_info, strlen(hkdf_info), exp, 112) != 0) {
        wa_log("chat: hkdf for %s failed", mms_type);
        return -1;
    }
    const uint8_t *iv = exp;
    const uint8_t *cipher_key = exp + 16;
    const uint8_t *mac_key = exp + 48;

    wa_buf ct;
    wa_buf_init(&ct);
    if (wa_aes_cbc_encrypt(cipher_key, iv, raw, raw_len, &ct) != 0) {
        wa_buf_free(&ct);
        wa_log("chat: aes cbc encrypt failed");
        return -1;
    }

    const void *parts[2] = { iv, ct.data };
    size_t lens[2] = { 16, ct.len };
    uint8_t mac[32];
    wa_hmac_sha256_v(mac_key, 32, 2, parts, lens, mac);
    wa_buf_append(&ct, mac, 10);

    wa_sha256(raw, raw_len, file_sha_out);
    wa_sha256(ct.data, ct.len, file_enc_sha_out);

    char b64_token[64];
    size_t b64_len = wa_base64_encode(file_enc_sha_out, 32, b64_token, sizeof b64_token);
    for (size_t i = 0; i < b64_len; i++) {
        if (b64_token[i] == '+') b64_token[i] = '-';
        else if (b64_token[i] == '/') b64_token[i] = '_';
    }
    while (b64_len > 0 && b64_token[b64_len - 1] == '=') {
        b64_token[--b64_len] = 0;
    }

    char path[1024];
    snprintf(path, sizeof path, "/mms/%s/%s?auth=%s&token=%s", mms_type, b64_token, auth_copy, b64_token);
    int http_st = 0;
    char *resp_json = wa_https_post(hostname, path, NULL, ct.data, ct.len, "application/octet-stream", 30000, &http_st);
    wa_buf_free(&ct);
    if (!resp_json || http_st < 200 || http_st >= 300) {
        wa_log("chat: media upload failed (http %d)", http_st);
        free(resp_json);
        return -1;
    }

    if (direct_path_out && dp_cap) direct_path_out[0] = 0;
    if (media_url_out && url_cap) media_url_out[0] = 0;
    char *dp_ptr = strstr(resp_json, "\"direct_path\":\"");
    if (dp_ptr && direct_path_out && dp_cap) {
        dp_ptr += 15;
        char *dp_end = strchr(dp_ptr, '"');
        if (dp_end) {
            size_t n = (size_t)(dp_end - dp_ptr);
            if (n < dp_cap) {
                memcpy(direct_path_out, dp_ptr, n);
                direct_path_out[n] = 0;
            }
        }
    }
    char *url_ptr = strstr(resp_json, "\"url\":\"");
    if (url_ptr && media_url_out && url_cap) {
        url_ptr += 7;
        char *url_end = strchr(url_ptr, '"');
        if (url_end) {
            size_t n = (size_t)(url_end - url_ptr);
            if (n < url_cap) {
                memcpy(media_url_out, url_ptr, n);
                media_url_out[n] = 0;
            }
        }
    }
    free(resp_json);
    if (!direct_path_out || !direct_path_out[0]) {
        wa_log("chat: upload response missing direct_path");
        return -1;
    }
    return 0;
}

int wa_chat_send_image(wa_client *c, wa_sess_store *sessions, const char *to_user_jid,
                       const uint8_t *jpeg, size_t jpeg_len,
                       const uint8_t *thumb, size_t thumb_len,
                       const char *caption,
                       const char *msg_id, char *out_msg_id, size_t out_msg_id_cap)
{
    if (!c || !sessions || !to_user_jid || !jpeg || !jpeg_len) return -1;

    uint8_t media_key[32], file_sha[32], file_enc_sha[32];
    char direct_path[256] = { 0 }, media_url[512] = { 0 };
    if (upload_media_blob(c, jpeg, jpeg_len, "WhatsApp Image Keys", "image",
                          media_key, file_sha, file_enc_sha,
                          direct_path, sizeof direct_path, media_url, sizeof media_url) != 0)
        return -1;

    wa_buf img;
    wa_buf_init(&img);
    if (media_url[0]) pb_put_string(&img, 1, media_url);
    pb_put_string(&img, 2, "image/jpeg");
    if (caption && caption[0]) pb_put_string(&img, 3, caption);
    pb_put_bytes(&img, 4, file_sha, 32);
    pb_put_varint(&img, 5, (uint64_t)jpeg_len);
    pb_put_bytes(&img, 8, media_key, 32);
    pb_put_bytes(&img, 9, file_enc_sha, 32);
    pb_put_string(&img, 11, direct_path);
    if (thumb && thumb_len) pb_put_bytes(&img, 16, thumb, thumb_len);

    wa_buf msg;
    wa_buf_init(&msg);
    pb_put_message(&msg, 3, &img);
    wa_buf_free(&img);

    int rc = wa_chat_send_message_payload(c, sessions, to_user_jid, msg.data, msg.len, msg_id, out_msg_id, out_msg_id_cap);
    wa_buf_free(&msg);
    return rc;
}

int wa_chat_send_audio(wa_client *c, wa_sess_store *sessions, const char *to_user_jid,
                       const uint8_t *audio, size_t audio_len,
                       uint32_t seconds, const char *mimetype,
                       const char *msg_id, char *out_msg_id, size_t out_msg_id_cap)
{
    if (!c || !sessions || !to_user_jid || !audio || !audio_len) return -1;

    uint8_t media_key[32], file_sha[32], file_enc_sha[32];
    char direct_path[256] = { 0 }, media_url[512] = { 0 };
    if (upload_media_blob(c, audio, audio_len, "WhatsApp Audio Keys", "audio",
                          media_key, file_sha, file_enc_sha,
                          direct_path, sizeof direct_path, media_url, sizeof media_url) != 0)
        return -1;

    wa_buf aud;
    wa_buf_init(&aud);
    if (media_url[0]) pb_put_string(&aud, 1, media_url);
    pb_put_string(&aud, 2, (mimetype && mimetype[0]) ? mimetype : "audio/mp4");
    pb_put_bytes(&aud, 3, file_sha, 32);
    pb_put_varint(&aud, 4, (uint64_t)audio_len);
    pb_put_varint(&aud, 5, (uint64_t)(seconds > 0 ? seconds : 1));
    pb_put_varint(&aud, 6, 1); // ptt = true
    pb_put_bytes(&aud, 7, media_key, 32);
    pb_put_bytes(&aud, 8, file_enc_sha, 32);
    pb_put_string(&aud, 9, direct_path);

    wa_buf msg;
    wa_buf_init(&msg);
    pb_put_message(&msg, 8, &aud); // Message.audioMessage = 8
    wa_buf_free(&aud);

    int rc = wa_chat_send_message_payload(c, sessions, to_user_jid, msg.data, msg.len, msg_id, out_msg_id, out_msg_id_cap);
    wa_buf_free(&msg);
    return rc;
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

// Send standard delivery receipt to recipient/group
static void send_delivery_receipt(wa_client *c, const char *to, const char *msg_id, const char *participant)
{
    if (!c || !to || !msg_id) return;
    wa_node *r = wa_node_new("receipt");
    wa_node_attr(r, "to", to);
    wa_node_attr(r, "id", msg_id);
    if (participant && participant[0]) wa_node_attr(r, "participant", participant);
    wa_client_send(c, r);
    wa_node_free(r);
}

int wa_chat_handle_message_full(wa_client *c, wa_sess_store *sessions, const wa_node *message,
                                char *frombuf, size_t fromcap,
                                wa_history_msg *out_msg,
                                wa_history_result *hist, int *is_history)
{
    if (hist) memset(hist, 0, sizeof *hist);
    if (is_history) *is_history = 0;
    if (out_msg) memset(out_msg, 0, sizeof *out_msg);
    wa_msg_incoming in;
    if (!wa_msg_parse_incoming(message, &in) || !in.from) return 0;
    wa_jid_to_non_ad(in.from, frombuf, fromcap);

    const char *part = wa_node_get(message, "participant");
    const char *sender_dev_jid = (part && part[0]) ? part : in.from;

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
    wa_sig_session *s = wa_sess_get(sessions, sender_dev_jid);
    wa_sig_session *use = s;
    if (is_prekey || !use) { use = &fresh; if (s) fresh = *s; else memset(&fresh, 0, sizeof fresh); }

    wa_buf msg;
    wa_buf_init(&msg);
    int rc = wa_msg_decrypt(use, &id, &spk, otp_ptr, is_prekey, in.enc, in.enc_len, &msg);
    if (rc == 0) wa_sess_put(sessions, sender_dev_jid, use);   // persist the advanced/established session

    // A linked device's initial history arrives as a peer protocolMessage/historySyncNotification.
    if (rc == 0 && hist) {
        int h = wa_history_from_message(msg.data, msg.len, NULL /* CDN TLS unverified; blob HMAC authenticates it */, hist);
        if (h != 0) {
            send_hist_sync_receipt(c, in.id);
            if (is_history) *is_history = (h == 1);
            wa_buf_free(&msg);
            return 0;
        }
    }

    int ret = 0;
    if (rc == 0 && out_msg) {
        char *dsm_dest = wa_msg_parse_dsm_dest(msg.data, msg.len);
        int is_dsm = 0;
        if (dsm_dest && dsm_dest[0]) {
            wa_jid_to_non_ad(dsm_dest, frombuf, fromcap);
            is_dsm = 1;
        }
        free(dsm_dest);

        char react_tid[96] = {0}, react_emoji[32] = {0};
        if (wa_msg_parse_reaction(msg.data, msg.len, react_tid, sizeof react_tid, react_emoji, sizeof react_emoji)) {
            out_msg->kind = WA_MSG_REACTION;
            out_msg->quoted_id = wa_strdup(react_tid);
            out_msg->text = wa_strdup(react_emoji);
            out_msg->from_me = is_dsm;
            ret = 1;
        } else if (wa_history_msg_parse(msg.data, msg.len, out_msg)) {
            if (!out_msg->id && in.id) out_msg->id = wa_strdup(in.id);
            const char *t_str = wa_node_get(message, "t");
            if (t_str && out_msg->timestamp == 0) out_msg->timestamp = (uint64_t)strtoull(t_str, NULL, 10);
            if (is_dsm) {
                out_msg->from_me = 1;
            } else if (in.notify && !out_msg->sender) {
                out_msg->sender = wa_strdup(in.notify);
            }
            ret = 1;
        }
    }

    if (rc == 0) {
        // Send delivery receipt back to sender only when decryption succeeded
        send_delivery_receipt(c, in.from, in.id, part);
    } else {
        wa_log("chat: decrypt failed (rc=%d) from=%s id=%s type=%s", rc, in.from, in.id ? in.id : "?", in.enc_type);
    }

    wa_buf_free(&msg);
    return ret;
}

char *wa_chat_handle_message(wa_client *c, wa_sess_store *sessions, const wa_node *message, char *frombuf, size_t fromcap,
                             wa_history_result *hist, int *is_history)
{
    wa_history_msg m;
    memset(&m, 0, sizeof m);
    int ok = wa_chat_handle_message_full(c, sessions, message, frombuf, fromcap, &m, hist, is_history);
    char *text = NULL;
    if (ok && m.text) text = wa_strdup(m.text);
    wa_history_msg_free(&m);
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
