#include "wa_callengine.h"
#include "wa_call.h"
#include "wa_chat.h"
#include "wa_user.h"
#include "wa_message.h"
#include "wa_signal.h"
#include "wa_store.h"
#include "wa_proto.h"
#include "wa_util.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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

int wa_callengine_place(wa_client *c, wa_sess_store *sessions, const char *to_user_jid, int video, wa_call_state *st)
{
    memset(st, 0, sizeof *st);
    if (wa_call_new_key(st->call_key) != 0) return -1;
    wa_call_new_id(st->call_id, sizeof st->call_id);
    snprintf(st->peer, sizeof st->peer, "%s", to_user_jid);
    st->video = video;

    wa_device_list devs;
    memset(&devs, 0, sizeof devs);
    if (resolve_devices(c, to_user_jid, &devs) != 0 || devs.ndevices == 0) return -1;

    const char *dest_user = to_user_jid;
    if (devs.lid[0] && strstr(to_user_jid, "@s.whatsapp.net")) {
        dest_user = devs.lid;
        snprintf(st->peer, sizeof st->peer, "%s", dest_user);
        wa_jid rlj;
        if (wa_jid_parse(devs.lid, &rlj) == 0) {
            for (int i = 0; i < devs.ndevices; i++) {
                wa_jid dj;
                if (wa_jid_parse(devs.jids[i], &dj) == 0) {
                    wa_jid_format(devs.jids[i], sizeof devs.jids[0], rlj.user, dj.device, "lid");
                }
            }
        }
    }

    wa_call_devkey keys[WA_MAX_DEVICES];
    wa_buf encbufs[WA_MAX_DEVICES];
    char typestr[WA_MAX_DEVICES][8];
    int nkeys = 0, any_pkmsg = 0;
    for (int i = 0; i < devs.ndevices && nkeys < WA_MAX_DEVICES; i++) {
        wa_buf_init(&encbufs[nkeys]);
        int is_prekey = 0;
        if (wa_chat_encrypt_callkey(c, sessions, devs.jids[i], st->call_key, &is_prekey, &encbufs[nkeys]) != 0) { wa_buf_free(&encbufs[nkeys]); continue; }
        snprintf(typestr[nkeys], 8, "%s", is_prekey ? "pkmsg" : "msg");
        keys[nkeys].device_jid = devs.jids[i];
        keys[nkeys].ciphertext = encbufs[nkeys].data;
        keys[nkeys].ciphertext_len = encbufs[nkeys].len;
        keys[nkeys].enc_type = typestr[nkeys];
        if (is_prekey) any_pkmsg = 1;
        nkeys++;
    }
    int rc = -1;
    if (nkeys > 0) {
        wa_device *d = wa_client_device(c);
        wa_call_offer o;
        memset(&o, 0, sizeof o);
        o.call_id = st->call_id;
        o.to = dest_user;
        o.call_creator = (strstr(dest_user, "@lid") && d->lid[0]) ? d->lid : d->pn;
        o.keys = keys;
        o.nkeys = nkeys;
        o.video = video;
        if (any_pkmsg) { o.device_identity = d->account; o.device_identity_len = d->account_len; }
        wa_node *offer = wa_call_build_offer(&o);
        rc = wa_client_send(c, offer);
        wa_node_free(offer);
        st->active = rc == 0;
    }
    for (int i = 0; i < nkeys; i++) wa_buf_free(&encbufs[i]);
    return rc;
}

int wa_callengine_on_offer(wa_client *c, wa_sess_store *sessions, const wa_node *call, wa_call_state *st)
{
    wa_call_incoming in;
    if (!wa_call_parse_offer(call, &in)) return 0;
    memset(st, 0, sizeof *st);
    if (in.call_id) snprintf(st->call_id, sizeof st->call_id, "%s", in.call_id);
    if (in.call_creator) snprintf(st->peer, sizeof st->peer, "%s", in.call_creator);
    else if (in.from) snprintf(st->peer, sizeof st->peer, "%s", in.from);
    st->video = in.video;

    wa_device *d = wa_client_device(c);

    // Send <ack class="call"> for the incoming offer stanza
    const char *stanza_id = wa_node_get(call, "id");
    if (stanza_id && in.from) {
        wa_node *ack = wa_node_new("ack");
        wa_node_attr(ack, "to", in.from);
        wa_node_attr(ack, "id", stanza_id);
        wa_node_attr(ack, "class", "call");
        wa_client_send(c, ack);
        wa_node_free(ack);
    }

    // Find the <enc> node specifically addressed to our device JID inside <destination> if present
    const wa_node *enc_for_us = in.enc;
    const wa_node *offer = wa_node_child(call, "offer");
    const wa_node *dest = offer ? wa_node_child(offer, "destination") : NULL;
    if (dest) {
        wa_jid me_pn, me_lid;
        int have_pn = wa_jid_parse(d->pn, &me_pn) == 0;
        int have_lid = wa_jid_parse(d->lid, &me_lid) == 0;
        for (int i = 0; i < dest->nchildren; i++) {
            const wa_node *to = dest->children[i];
            if (!wa_node_tag_is(to, "to")) continue;
            const char *tjid = wa_node_get(to, "jid");
            if (!tjid) continue;
            if ((d->pn[0] && strcmp(tjid, d->pn) == 0) || (d->lid[0] && strcmp(tjid, d->lid) == 0)) {
                const wa_node *e = wa_node_child(to, "enc");
                if (e && e->has_content) { enc_for_us = e; break; }
            }
            wa_jid tj;
            if (wa_jid_parse(tjid, &tj) == 0) {
                if ((have_pn && tj.device == me_pn.device && strcmp(tj.user, me_pn.user) == 0) ||
                    (have_lid && tj.device == me_lid.device && strcmp(tj.user, me_lid.user) == 0)) {
                    const wa_node *e = wa_node_child(to, "enc");
                    if (e && e->has_content) { enc_for_us = e; break; }
                }
            }
        }
    }

    // Decrypt the call key from the <enc> addressed to us (Message{call{callKey}}).
    if (enc_for_us && enc_for_us->has_content) {
        const char *etype = wa_node_get_or(enc_for_us, "type", "msg");
        int is_prekey = strcmp(etype, "pkmsg") == 0;
        wa_sig_keypair id, spk, otp;
        const wa_sig_keypair *otp_ptr = NULL;
        memcpy(id.priv, d->identity_key.priv, 32); memcpy(id.pub, d->identity_key.pub, 32);
        memcpy(spk.priv, d->signed_pre_key.priv, 32); memcpy(spk.pub, d->signed_pre_key.pub, 32);
        if (is_prekey) {
            uint32_t reg_id = 0, prekey_id = 0, spk_id = 0;
            int has_pk = 0;
            uint8_t base_pub[32], id_pub[32];
            if (wa_sig_prekey_header(enc_for_us->content, enc_for_us->content_len,
                                     &reg_id, &has_pk, &prekey_id, &spk_id, base_pub, id_pub) == 0 && has_pk) {
                const wa_keypair *k = wa_device_find_prekey(d, prekey_id);
                if (k) { memcpy(otp.priv, k->priv, 32); memcpy(otp.pub, k->pub, 32); otp_ptr = &otp; }
            }
        }
        wa_sig_session fresh, *s = st->peer[0] ? wa_sess_get(sessions, st->peer) : NULL;
        wa_sig_session *use = &fresh;
        if (s && !is_prekey) fresh = *s; else memset(&fresh, 0, sizeof fresh);
        wa_buf msg;
        wa_buf_init(&msg);
        if (wa_msg_decrypt(use, &id, &spk, otp_ptr, is_prekey, enc_for_us->content, enc_for_us->content_len, &msg) == 0) {
            const uint8_t *callnode, *key;
            size_t cl, kl;
            if (pb_get_bytes(msg.data, msg.len, 10, &callnode, &cl) && pb_get_bytes(callnode, cl, 1, &key, &kl) && kl == 32) {
                memcpy(st->call_key, key, 32);
                st->active = 1;
                if (st->peer[0]) wa_sess_put(sessions, st->peer, use);
            }
        }
        wa_buf_free(&msg);
    }

    // Send <preaccept> so caller knows our companion device is ringing and supports Opus
    if (st->call_id[0] && st->peer[0]) {
        wa_node *pa = wa_call_build_preaccept(st->call_id, in.from ? in.from : st->peer, st->peer, NULL, st->video);
        wa_client_send(c, pa);
        wa_node_free(pa);
    }
    return 1;
}

int wa_callengine_accept(wa_client *c, const wa_call_state *st)
{
    const char *rates[] = { "8000", "16000" };
    wa_node *n = wa_call_build_accept(st->call_id, st->peer, st->peer, rates, 2, NULL, 0, st->video);
    int rc = wa_client_send(c, n);
    wa_node_free(n);
    return rc;
}

int wa_callengine_reject(wa_client *c, const wa_call_state *st)
{
    wa_node *n = wa_call_build_reject(st->call_id, st->peer, st->peer);
    int rc = wa_client_send(c, n);
    wa_node_free(n);
    return rc;
}

int wa_callengine_terminate(wa_client *c, const wa_call_state *st, const char *reason)
{
    wa_node *n = wa_call_build_terminate(st->call_id, st->peer, st->peer, reason);
    int rc = wa_client_send(c, n);
    wa_node_free(n);
    return rc;
}
