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
    if (resolve_devices(c, to_user_jid, &devs) != 0 || devs.ndevices == 0) return -1;

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
        o.to = to_user_jid;
        o.call_creator = d->pn;
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

    // Decrypt the call key from the <enc> addressed to us (Message{call{callKey}}).
    if (in.enc && in.enc->has_content) {
        const char *etype = wa_node_get_or(in.enc, "type", "msg");
        int is_prekey = strcmp(etype, "pkmsg") == 0;
        wa_sig_keypair id, spk;
        wa_device *d = wa_client_device(c);
        memcpy(id.priv, d->identity_key.priv, 32); memcpy(id.pub, d->identity_key.pub, 32);
        memcpy(spk.priv, d->signed_pre_key.priv, 32); memcpy(spk.pub, d->signed_pre_key.pub, 32);
        wa_sig_session fresh, *s = st->peer[0] ? wa_sess_get(sessions, st->peer) : NULL;
        wa_sig_session *use = &fresh;
        if (s && !is_prekey) fresh = *s; else memset(&fresh, 0, sizeof fresh);
        wa_buf msg;
        wa_buf_init(&msg);
        if (wa_msg_decrypt(use, &id, &spk, NULL, is_prekey, in.enc->content, in.enc->content_len, &msg) == 0) {
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
