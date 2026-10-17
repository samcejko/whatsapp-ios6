#import "WASession.h"
#import "WACommon.h"
#import "WAChatStore.h"

#import <AudioToolbox/AudioToolbox.h>
#include "wa_store.h"
#include "wa_client.h"
#include "wa_sessions.h"
#include "wa_chat.h"
#include "wa_history.h"
#include "wa_callengine.h"
#include "wa_net.h"
#include "wa_rtc.h"
#include "wa_wstun.h"
#include "wa_srtp.h"
#include "wa_relay.h"

NSString *const WASessionStateChanged = @"WASessionStateChanged";
NSString *const WASessionQRChanged = @"WASessionQRChanged";
NSString *const WASessionPaired = @"WASessionPaired";
NSString *const WASessionNodeReceived = @"WASessionNodeReceived";
NSString *const WASessionMessage = @"WASessionMessage";
NSString *const WASessionMessageStatus = @"WASessionMessageStatus";
NSString *const WASessionConversations = @"WASessionConversations";
NSString *const WASessionPresenceChanged = @"WASessionPresenceChanged";
NSString *const WASessionIncomingCall = @"WASessionIncomingCall";
NSString *const WASessionCallAccepted = @"WASessionCallAccepted";
NSString *const WASessionCallEnded = @"WASessionCallEnded";
NSString *const WASessionCallAudioPacket = @"WASessionCallAudioPacket";

@interface WASession () {
@public
    wa_device _dev;
    wa_client *_client;
    wa_client_callbacks _cb;
    wa_client_config _cfg;
    wa_sess_store *_sessions;   // Signal sessions, touched only on the client thread
    NSThread *_thread;
    wa_call_state _currentCall;
    int _hasCurrentCall;
    wa_rtc *_rtc;
}
@property (nonatomic) WASessionState state;
@property (nonatomic, copy) NSString *qrString;
@property (nonatomic, copy) NSString *ownJID;
@property (nonatomic) BOOL running;
@end

static NSString *storePath(void) { return [WADocumentsPath() stringByAppendingPathComponent:@"wa6.store"]; }
static NSString *sessionsPath(void) { return [WADocumentsPath() stringByAppendingPathComponent:@"wa6.sessions"]; }

// ---- C callbacks (called on the client thread; marshalled to the main thread) ----

static void cb_qr(void *ctx, const char *qr)
{
    WASession *self = (__bridge WASession *)ctx;
    NSString *s = qr ? [NSString stringWithUTF8String:qr] : nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        self.qrString = s;
        self.state = WASessionStatePairing;
        if (s) [[NSNotificationCenter defaultCenter] postNotificationName:WASessionQRChanged object:self userInfo:@{ @"qr": s }];
        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionStateChanged object:self userInfo:@{ @"state": @(WASessionStatePairing) }];
    });
}

static void cb_paired(void *ctx, const char *jid)
{
    WASession *self = (__bridge WASession *)ctx;
    NSString *j = jid ? [NSString stringWithUTF8String:jid] : @"";
    dispatch_async(dispatch_get_main_queue(), ^{
        self.ownJID = j;
        self.qrString = nil;
        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionPaired object:self userInfo:@{ @"jid": j }];
    });
}

static void cb_connected(void *ctx)
{
    WASession *self = (__bridge WASession *)ctx;
    dispatch_async(dispatch_get_main_queue(), ^{
        self.state = WASessionStateConnected;
        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionStateChanged object:self userInfo:@{ @"state": @(WASessionStateConnected) }];
        // Automatically query group metadata for any group chats and own profile picture
        if (self.ownJID.length) [self fetchProfilePicture:self.ownJID];
        for (NSDictionary *c in [[WAChatStore shared] conversationsSorted]) {
            NSString *cjid = c[@"jid"];
            if ([cjid hasSuffix:@"@g.us"]) {
                [self fetchGroupMetadata:cjid];
            }
        }
    });
}

static void rtc_on_open(void *ctx)
{
    WASession *self = (__bridge WASession *)ctx;
    WALog(@"call: WebRTC data channel opened, sending STUN ALLOCATE & ping");
    if (!self->_rtc || !self->_currentCall.has_relay) return;

    const wa_relay_endpoint *ep = wa_relay_pick_endpoint(&self->_currentCall.relay, 1);
    if (!ep) return;

    int tok_idx = (ep->token_id >= 0 && ep->token_id < self->_currentCall.relay.ntokens) ? ep->token_id : 0;
    const uint8_t *tok = self->_currentCall.relay.tokens[tok_idx];
    size_t tok_len = self->_currentCall.relay.token_len[tok_idx];

    const uint8_t *mi = self->_currentCall.relay.has_relay_key ? self->_currentCall.relay.relay_key
                       : (self->_currentCall.relay.has_hbh_key ? self->_currentCall.relay.hbh_key : NULL);
    size_t mi_len = self->_currentCall.relay.has_relay_key ? self->_currentCall.relay.relay_key_len
                   : (self->_currentCall.relay.has_hbh_key ? 30 : 0);

    uint8_t tid[12];
    wa_random(tid, 12);
    wa_buf alloc_req;
    wa_buf_init(&alloc_req);
    if (wa_stun_build_wasm_allocate(&alloc_req, tid, tok, tok_len, ep->ipv4, ep->port, mi, mi_len) == 0) {
        wa_rtc_send(self->_rtc, alloc_req.data, alloc_req.len);
    }
    wa_buf_free(&alloc_req);

    // Keepalive ping
    uint8_t ping[20];
    wa_random(tid, 12);
    wa_stun_ping(ping, tid);
    wa_rtc_send(self->_rtc, ping, 20);
}

static void rtc_on_packet(void *ctx, const uint8_t *data, size_t len)
{
    WASession *self = (__bridge WASession *)ctx;
    if (!data || len < 4) return;

    if (wa_stun_is_stun(data, len)) {
        uint16_t type = 0;
        wa_stun_type(data, len, &type);
        if (type == WA_STUN_PING) {
            uint8_t pong[20];
            const uint8_t *tid = wa_stun_tid(data, len);
            if (tid) {
                wa_put_u16(pong, WA_STUN_PONG);
                wa_put_u16(pong + 2, 0);
                wa_put_u32(pong + 4, 0x2112a442u);
                memcpy(pong + 8, tid, 12);
                wa_rtc_send(self->_rtc, pong, 20);
            }
        }
        return;
    }

    if ((data[0] & 0xC0) == 0x80 && len >= 16 && self->_currentCall.srtp_ready) {
        uint8_t cc = data[0] & 0x0F;
        size_t hdr_len = 12 + cc * 4;
        if (len <= hdr_len) return;

        size_t tag_len = self->_currentCall.relay.warp_mi_tag_len ? self->_currentCall.relay.warp_mi_tag_len : WA_WARP_TAG_LEN;
        if (len < hdr_len + tag_len) return;

        size_t payload_len = len - hdr_len - tag_len;
        uint16_t seq = wa_get_u16(data + 2);
        uint32_t ssrc = wa_get_u32(data + 8);
        uint32_t roc = wa_roc_recv_estimate(&self->_currentCall.roc_recv, seq);
        wa_roc_recv_commit(&self->_currentCall.roc_recv, roc, seq);

        uint8_t payload[1500];
        if (payload_len <= sizeof payload) {
            memcpy(payload, data + hdr_len, payload_len);
            if (wa_srtp_crypt_payload(&self->_currentCall.recv_keys, ssrc, roc, seq, payload, payload_len) == 0) {
                NSData *payloadData = [NSData dataWithBytes:payload length:payload_len];
                dispatch_async(dispatch_get_main_queue(), ^{
                    [[NSNotificationCenter defaultCenter] postNotificationName:WASessionCallAudioPacket
                                                                        object:self
                                                                      userInfo:@{ @"payload": payloadData }];
                });
            }
        }
    }
}

static void rtc_on_closed(void *ctx, const char *reason)
{
    WASession *self = (__bridge WASession *)ctx;
    WALog(@"call: media transport closed (%s)", reason ? reason : "normal");
    self->_rtc = NULL;
}

static void start_call_media(WASession *self)
{
    if (!self->_hasCurrentCall || self->_rtc || !self->_currentCall.has_relay) return;

    const wa_relay_endpoint *ep = wa_relay_pick_endpoint(&self->_currentCall.relay, 1);
    if (!ep || !ep->ipv4[0]) {
        WALog(@"call: no suitable relay endpoint found");
        return;
    }

    const char *our_lid = self->_dev.lid[0] ? self->_dev.lid : self->_dev.pn;
    const char *peer_jid = self->_currentCall.peer;
    wa_callengine_setup_srtp(&self->_currentCall, our_lid, peer_jid);

    WALog(@"call: connecting media transport to %s:%u", ep->ipv4, ep->port);
    wa_rtc_callbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = (__bridge void *)self;
    cb.on_open = rtc_on_open;
    cb.on_packet = rtc_on_packet;
    cb.on_closed = rtc_on_closed;

    self->_rtc = wa_rtc_connect(ep->ipv4, ep->port, &cb);
    if (!self->_rtc) {
        WALog(@"call: wa_rtc_connect failed");
    }
}

// Acknowledge a received stanza so the server stops redelivering it.
static void send_ack(WASession *self, const wa_node *node, const char *cls)
{
    const char *id = wa_node_get(node, "id");
    const char *from = wa_node_get(node, "from");
    if (!id || !from) return;
    wa_node *ack = wa_node_new("ack");
    wa_node_attr(ack, "to", from);
    wa_node_attr(ack, "id", id);
    wa_node_attr(ack, "class", cls);
    const char *participant = wa_node_get(node, "participant");
    if (participant) wa_node_attr(ack, "participant", participant);
    wa_client_send(self->_client, ack);
    wa_node_free(ack);
}

// Runs on the client thread (from process_frames).
static void cb_node(void *ctx, const wa_node *node)
{
    WASession *self = (__bridge WASession *)ctx;

    if (wa_node_tag_is(node, "message")) {
        const char *notify = wa_node_get(node, "notify");
        const char *raw_from = wa_node_get(node, "from");
        const char *raw_part = wa_node_get(node, "participant");
        if (notify && notify[0]) {
            NSString *pName = [NSString stringWithUTF8String:notify];
            const char *target = (raw_part && raw_part[0]) ? raw_part : raw_from;
            if (target && target[0] && !strstr(target, "@g.us")) {
                NSString *pJid = [NSString stringWithUTF8String:target];
                if (pName.length && pJid.length) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [[WAChatStore shared] setPushName:pName forJID:pJid];
                    });
                }
            }
        }

        char from[80] = { 0 };
        wa_history_result hist;
        int is_history = 0;
        wa_history_msg live_msg;
        memset(&live_msg, 0, sizeof live_msg);
        int ok = wa_chat_handle_message_full(self->_client, self->_sessions, node, from, sizeof from, &live_msg, &hist, &is_history);
        wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
        send_ack(self, node, "message");
        if (is_history && (hist.nconvs > 0 || hist.npushnames > 0 || hist.nmappings > 0)) {
            NSMutableArray *convs = [NSMutableArray arrayWithCapacity:hist.nconvs];
            for (int i = 0; i < hist.nconvs; i++) {
                wa_history_conv *cv = &hist.convs[i];
                NSString *jid = cv->jid ? [NSString stringWithUTF8String:cv->jid] : nil;
                if (!jid) continue;
                NSString *name = cv->name ? [NSString stringWithUTF8String:cv->name] : nil;
                NSString *last = cv->last ? [NSString stringWithUTF8String:cv->last] : nil;
                NSString *pnJid = cv->pn_jid ? [NSString stringWithUTF8String:cv->pn_jid] : nil;
                NSString *lidJid = cv->lid_jid ? [NSString stringWithUTF8String:cv->lid_jid] : nil;
                NSMutableArray *msgs = [NSMutableArray arrayWithCapacity:cv->nmsgs];
                for (int j = 0; j < cv->nmsgs; j++) {
                    wa_history_msg *mm = &cv->msgs[j];
                    NSString *mt = mm->text ? ([NSString stringWithUTF8String:mm->text] ?: @"") : @"";
                    NSMutableDictionary *md = [NSMutableDictionary dictionaryWithObjectsAndKeys:
                                               mt, @"text",
                                               @(mm->from_me != 0), @"out",
                                               @(mm->timestamp), @"ts",
                                               @(mm->kind), @"kind", nil];
                    int st = mm->status;
                    if (mm->from_me && st == 0) st = WA_STATUS_DELIVERY_ACK;
                    md[@"status"] = @(st);
                    NSString *mid = mm->id ? [NSString stringWithUTF8String:mm->id] : nil;
                    if (mid) md[@"id"] = mid;
                    if (mm->duration) md[@"dur"] = @(mm->duration);
                    if (mm->filename) { NSString *fn = [NSString stringWithUTF8String:mm->filename]; if (fn) md[@"file"] = fn; }
                    if (mm->thumb && mm->thumb_len) md[@"thumb"] = [NSData dataWithBytes:mm->thumb length:mm->thumb_len];
                    if (mm->sender) { NSString *sn = [NSString stringWithUTF8String:mm->sender]; if (sn.length) md[@"sender"] = sn; }
                    if (mm->direct_path) { NSString *dp = [NSString stringWithUTF8String:mm->direct_path]; if (dp.length) md[@"dpath"] = dp; }
                    if (mm->media_key && mm->media_key_len) md[@"mkey"] = [NSData dataWithBytes:mm->media_key length:mm->media_key_len];
                    if (mm->enc_sha && mm->enc_sha_len) md[@"esha"] = [NSData dataWithBytes:mm->enc_sha length:mm->enc_sha_len];
                    if (mm->quoted_id) {
                        NSString *qid = [NSString stringWithUTF8String:mm->quoted_id];
                        NSString *qtxt = mm->quoted_text ? [NSString stringWithUTF8String:mm->quoted_text] : @"";
                        if (qid.length) md[@"quoted"] = @{ @"id": qid, @"text": (qtxt ?: @"") };
                    }
                    [msgs addObject:md];
                }
                [convs addObject:@{ @"jid": jid,
                                    @"name": name ?: @"",
                                    @"last": last ?: @"",
                                    @"pnJid": pnJid ?: @"",
                                    @"lidJid": lidJid ?: @"",
                                    @"ts": @(cv->timestamp),
                                    @"unread": @(cv->unread),
                                    @"messages": msgs }];
            }
            NSMutableArray *pnames = [NSMutableArray arrayWithCapacity:hist.npushnames];
            for (int k = 0; k < hist.npushnames; k++) {
                NSString *pid = hist.pushnames[k].id ? [NSString stringWithUTF8String:hist.pushnames[k].id] : nil;
                NSString *pn = hist.pushnames[k].pushname ? [NSString stringWithUTF8String:hist.pushnames[k].pushname] : nil;
                if (pid.length && pn.length) {
                    [pnames addObject:@{ @"id": pid, @"name": pn }];
                }
            }
            NSMutableArray *maps = [NSMutableArray arrayWithCapacity:hist.nmappings];
            for (int k = 0; k < hist.nmappings; k++) {
                NSString *pn = hist.mappings[k].pn_jid ? [NSString stringWithUTF8String:hist.mappings[k].pn_jid] : nil;
                NSString *lid = hist.mappings[k].lid_jid ? [NSString stringWithUTF8String:hist.mappings[k].lid_jid] : nil;
                if (pn.length && lid.length) {
                    [maps addObject:@{ @"pnJid": pn, @"lidJid": lid }];
                }
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionConversations object:self userInfo:@{
                    @"conversations": convs,
                    @"pushnames": pnames,
                    @"mappings": maps
                }];
            });
        }
        wa_history_result_free(&hist);
        if (ok) {
            NSString *f = [NSString stringWithUTF8String:from];
            NSString *t = live_msg.text ? [NSString stringWithUTF8String:live_msg.text] : @"";
            if (live_msg.kind == WA_MSG_REACTION) {
                NSString *tid = live_msg.quoted_id ? [NSString stringWithUTF8String:live_msg.quoted_id] : nil;
                if (f.length && tid.length) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [[WAChatStore shared] addReaction:(t ?: @"") toMessageId:tid inChat:f];
                    });
                }
                wa_history_msg_free(&live_msg);
                return;
            }
            BOOL isOut = (live_msg.from_me != 0);
            BOOL isGroup = ([f rangeOfString:@"@g.us"].location != NSNotFound);
            NSMutableDictionary *md = [NSMutableDictionary dictionaryWithObjectsAndKeys:
                                       (f ?: @""), @"from",
                                       (t ?: @""), @"text",
                                       @(isOut), @"out",
                                       @(live_msg.timestamp ?: (unsigned long long)[[NSDate date] timeIntervalSince1970]), @"ts",
                                       @(live_msg.kind), @"kind",
                                       @(WA_STATUS_DELIVERY_ACK), @"status", nil];
            if (live_msg.id) { NSString *mid = [NSString stringWithUTF8String:live_msg.id]; if (mid) md[@"id"] = mid; }
            if (live_msg.duration) md[@"dur"] = @(live_msg.duration);
            if (live_msg.filename) { NSString *fn = [NSString stringWithUTF8String:live_msg.filename]; if (fn) md[@"file"] = fn; }
            if (live_msg.thumb && live_msg.thumb_len) md[@"thumb"] = [NSData dataWithBytes:live_msg.thumb length:live_msg.thumb_len];
            if (live_msg.sender) {
                NSString *sn = [NSString stringWithUTF8String:live_msg.sender];
                if (sn.length) {
                    md[@"sender"] = sn;
                    if (!isGroup && !isOut) md[@"pushName"] = sn;
                }
            }
            if (live_msg.direct_path) { NSString *dp = [NSString stringWithUTF8String:live_msg.direct_path]; if (dp.length) md[@"dpath"] = dp; }
            if (live_msg.media_key && live_msg.media_key_len) md[@"mkey"] = [NSData dataWithBytes:live_msg.media_key length:live_msg.media_key_len];
            if (live_msg.enc_sha && live_msg.enc_sha_len) md[@"esha"] = [NSData dataWithBytes:live_msg.enc_sha length:live_msg.enc_sha_len];
            if (live_msg.quoted_id) {
                NSString *qid = [NSString stringWithUTF8String:live_msg.quoted_id];
                NSString *qtxt = live_msg.quoted_text ? [NSString stringWithUTF8String:live_msg.quoted_text] : @"";
                if (qid.length) md[@"quoted"] = @{ @"id": qid, @"text": (qtxt ?: @"") };
            }
            const char *notify = wa_node_get(node, "notify");
            if (notify && !isGroup && !isOut) {
                NSString *pn = [NSString stringWithUTF8String:notify];
                if (pn.length) md[@"pushName"] = pn;
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                NSString *fromJid = md[@"from"];
                if (!isOut && fromJid.length && ![[WAChatStore shared] isChatMuted:fromJid]) {
                    SystemSoundID snd = (SystemSoundID)[[NSUserDefaults standardUserDefaults] integerForKey:@"wa_sound_id"];
                    if (snd == 0) snd = 1003;
                    AudioServicesPlaySystemSound(snd);
                }
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessage object:self userInfo:md];
            });
            wa_history_msg_free(&live_msg);
        }
        return;
    }
    if (wa_node_tag_is(node, "call")) {
        send_ack(self, node, "call");
        wa_call_state st;
        if (wa_callengine_on_offer(self->_client, self->_sessions, node, &st)) {
            wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
            self->_currentCall = st;
            self->_hasCurrentCall = 1;
            WALog(@"incoming call from %s (video=%d, key %s)", st.peer, st.video, st.active ? "ok" : "missing");
            char bare_peer[80] = {0};
            wa_jid_to_non_ad(st.peer, bare_peer, sizeof bare_peer);
            NSString *peer = [NSString stringWithUTF8String:bare_peer[0] ? bare_peer : st.peer];
            NSString *callId = [NSString stringWithUTF8String:st.call_id];
            BOOL video = (st.video != 0);
            dispatch_async(dispatch_get_main_queue(), ^{
                [[WAChatStore shared] recordCallLog:@{
                    @"jid": (peer ?: @""),
                    @"name": WADisplayNameForChat(peer, nil),
                    @"out": @NO,
                    @"video": @(video),
                    @"ts": @((unsigned long long)[[NSDate date] timeIntervalSince1970])
                }];
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionIncomingCall
                                                                    object:self
                                                                  userInfo:@{ @"jid": (peer ?: @""),
                                                                              @"callId": (callId ?: @""),
                                                                              @"video": @(video) }];
            });
            if (self->_currentCall.has_relay) {
                start_call_media(self);
            }
            return;
        }

        if (self->_hasCurrentCall) {
            if (wa_callengine_on_relay(node, &self->_currentCall)) {
                WALog(@"call: received relay info, starting media transport");
                start_call_media(self);
            }
        }

        for (int i = 0; i < node->nchildren; i++) {
            const wa_node *child = node->children[i];
            if (wa_node_tag_is(child, "accept") || wa_node_tag_is(child, "preaccept")) {
                const char *cid = wa_node_get(child, "call-id");
                NSString *callId = cid ? [NSString stringWithUTF8String:cid] : @"";
                if (self->_hasCurrentCall) {
                    if (wa_callengine_on_relay(child, &self->_currentCall)) {
                        start_call_media(self);
                    }
                    if (self->_currentCall.has_relay) {
                        start_call_media(self);
                    }
                }
                if (wa_node_tag_is(child, "accept")) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionCallAccepted
                                                                            object:self
                                                                          userInfo:@{ @"callId": (callId ?: @"") }];
                    });
                }
                return;
            }
            if (wa_node_tag_is(child, "terminate") || wa_node_tag_is(child, "reject")) {
                if (self->_rtc) {
                    wa_rtc_close(self->_rtc);
                    self->_rtc = NULL;
                }
                self->_hasCurrentCall = 0;
                const char *cid = wa_node_get(child, "call-id");
                const char *rsn = wa_node_get(child, "reason");
                NSString *callId = cid ? [NSString stringWithUTF8String:cid] : @"";
                NSString *reason = rsn ? [NSString stringWithUTF8String:rsn] : @"ended";
                dispatch_async(dispatch_get_main_queue(), ^{
                    [[NSNotificationCenter defaultCenter] postNotificationName:WASessionCallEnded
                                                                        object:self
                                                                      userInfo:@{ @"callId": (callId ?: @""), @"reason": (reason ?: @"") }];
                });
                return;
            }
        }
        return;
    }

    if (wa_node_tag_is(node, "ack")) {
        const char *cls = wa_node_get(node, "class");
        if (cls && strcmp(cls, "message") == 0) {
            const char *aid = wa_node_get(node, "id");
            const char *err = wa_node_get(node, "error");
            if (aid && aid[0]) {
                NSString *mid = [NSString stringWithUTF8String:aid];
                int st = err ? WA_STATUS_ERROR : WA_STATUS_SERVER_ACK;
                NSString *errStr = err ? [NSString stringWithUTF8String:err] : nil;
                if (mid) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        NSMutableDictionary *ui = [NSMutableDictionary dictionaryWithObjectsAndKeys:mid, @"id", @(st), @"status", nil];
                        if (errStr) ui[@"error"] = errStr;
                        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus object:self userInfo:ui];
                    });
                }
            }
        }
    }
    if (wa_node_tag_is(node, "receipt")) {
        send_ack(self, node, "receipt");
        const char *rtype = wa_node_get(node, "type");
        const char *from = wa_node_get(node, "from");
        if (rtype && strcmp(rtype, "retry") == 0) {
            WALog(@"receipt retry from %s - resetting signal session", from ? from : "(null)");
            if (from && from[0] && self->_sessions) {
                char bare_from[80] = {0};
                wa_jid_to_non_ad(from, bare_from, sizeof bare_from);
                wa_sess_del(self->_sessions, bare_from[0] ? bare_from : from);
                wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
            }
        }
        int st = WA_STATUS_DELIVERY_ACK;
        if (rtype) {
            if (strcmp(rtype, "read") == 0 || strcmp(rtype, "read-self") == 0) st = WA_STATUS_READ;
            else if (strcmp(rtype, "played") == 0 || strcmp(rtype, "played-self") == 0) st = WA_STATUS_PLAYED;
            else if (strcmp(rtype, "hist_sync") == 0) st = -1;
        }
        if (st >= 0) {
            const char *rid = wa_node_get(node, "id");
            if (rid && rid[0]) {
                NSString *mid = [NSString stringWithUTF8String:rid];
                if (mid) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus object:self userInfo:@{ @"id": mid, @"status": @(st) }];
                    });
                }
            }
            for (int i = 0; i < node->nchildren; i++) {
                const wa_node *child = node->children[i];
                if (wa_node_tag_is(child, "list")) {
                    for (int j = 0; j < child->nchildren; j++) {
                        const wa_node *item = child->children[j];
                        const char *iid = wa_node_get(item, "id");
                        if (iid && iid[0]) {
                            NSString *mid = [NSString stringWithUTF8String:iid];
                            if (mid) {
                                dispatch_async(dispatch_get_main_queue(), ^{
                                    [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus object:self userInfo:@{ @"id": mid, @"status": @(st) }];
                                });
                            }
                        }
                    }
                }
            }
        }
    }
    if (wa_node_tag_is(node, "presence")) {
        const char *from = wa_node_get(node, "from");
        const char *type = wa_node_get(node, "type");
        const char *last = wa_node_get(node, "last");
        if (from && from[0]) {
            NSString *f = [NSString stringWithUTF8String:from];
            NSString *t = type ? [NSString stringWithUTF8String:type] : @"available";
            NSMutableDictionary *ui = [NSMutableDictionary dictionaryWithObjectsAndKeys:f, @"jid", t, @"state", nil];
            if (last) ui[@"last"] = [NSString stringWithUTF8String:last];
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionPresenceChanged object:self userInfo:ui];
            });
        }
    }
    if (wa_node_tag_is(node, "chatstate")) {
        const char *from = wa_node_get(node, "from");
        if (from && from[0]) {
            NSString *f = [NSString stringWithUTF8String:from];
            NSString *state = @"paused";
            wa_node *comp = wa_node_child(node, "composing");
            if (comp) {
                const char *media = wa_node_get(comp, "media");
                if (media && strcmp(media, "audio") == 0) state = @"recording";
                else state = @"composing";
            }
            else if (wa_node_child(node, "recording")) state = @"recording";
            else if (wa_node_child(node, "paused")) state = @"paused";
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionPresenceChanged object:self userInfo:@{ @"jid": f, @"state": state }];
            });
        }
    }

    if (wa_node_tag_is(node, "iq")) {
        const char *from = wa_node_get(node, "from");
        const char *type = wa_node_get(node, "type");
        const char *id = wa_node_get(node, "id");
        if (type && strcmp(type, "result") == 0) {
            wa_node *pic = wa_node_child(node, "picture");
            if (pic) {
                NSString *key = nil;
                if (id && strncmp(id, "pic_", 4) == 0) {
                    NSString *rawKey = [NSString stringWithUTF8String:id + 4];
                    if ([rawKey hasSuffix:@"_pn"]) rawKey = [rawKey substringToIndex:rawKey.length - 3];
                    key = [WAChatStore keyForJID:rawKey];
                } else if (from && from[0]) {
                    key = [WAChatStore keyForJID:[NSString stringWithUTF8String:from]];
                }
                if (key.length) {
                    NSString *path = [WADocumentsPath() stringByAppendingPathComponent:[NSString stringWithFormat:@"wa6_avatar_%@.jpg", key]];
                    if (pic->has_content && pic->content_len > 4 && pic->content[0] == 0xFF && pic->content[1] == 0xD8) {
                        NSData *data = [NSData dataWithBytes:pic->content length:pic->content_len];
                        [data writeToFile:path atomically:YES];
                        dispatch_async(dispatch_get_main_queue(), ^{
                            [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": key }];
                        });
                    } else {
                        const char *dpath = wa_node_get(pic, "direct_path");
                        const char *url = wa_node_get(pic, "url");
                        NSString *dpStr = dpath ? [NSString stringWithUTF8String:dpath] : nil;
                        NSString *urlStr = url ? [NSString stringWithUTF8String:url] : nil;
                        if (dpStr.length || urlStr.length) {
                            dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0), ^{
                                NSString *host = @"pps.whatsapp.net";
                                NSString *reqPath = dpStr;
                                if (urlStr.length) {
                                    NSURL *u = [NSURL URLWithString:urlStr];
                                    if (u.host.length) host = u.host;
                                    if (u.path.length) {
                                        reqPath = u.query.length ? [NSString stringWithFormat:@"%@?%@", u.path, u.query] : u.path;
                                    }
                                }
                                if (reqPath.length) {
                                    int http_st = 0;
                                    size_t out_len = 0;
                                    uint8_t *bytes = wa_https_get_bin(host.UTF8String, reqPath.UTF8String, NULL, 15000, &http_st, &out_len);
                                    if (bytes && http_st == 200 && out_len > 4) {
                                        NSData *data = [NSData dataWithBytes:bytes length:out_len];
                                        [data writeToFile:path atomically:YES];
                                        dispatch_async(dispatch_get_main_queue(), ^{
                                            [[NSNotificationCenter defaultCenter] postNotificationName:WAChatStoreChanged object:self userInfo:@{ @"jid": key }];
                                        });
                                    }
                                    free(bytes);
                                }
                            });
                        }
                    }
                }
            }
            wa_node *grp = wa_node_child(node, "group");
            if (grp) {
                const char *gid = wa_node_get(grp, "id");
                const char *subj = wa_node_get(grp, "subject");
                NSString *gJid = nil;
                if (gid && gid[0]) {
                    NSString *rawG = [NSString stringWithUTF8String:gid];
                    gJid = [rawG hasSuffix:@"@g.us"] ? rawG : [rawG stringByAppendingString:@"@g.us"];
                } else if (from && strstr(from, "@g.us")) {
                    gJid = [NSString stringWithUTF8String:from];
                }
                NSString *subjStr = subj ? [NSString stringWithUTF8String:subj] : nil;
                NSMutableArray *parts = [NSMutableArray array];
                NSMutableArray *lidMappings = [NSMutableArray array];
                for (int i = 0; i < grp->nchildren; i++) {
                    const wa_node *ch = grp->children[i];
                    if (wa_node_tag_is(ch, "participant")) {
                        const char *pjid = wa_node_get(ch, "jid");
                        const char *ppn = wa_node_get(ch, "phone_number");
                        if (!ppn) ppn = wa_node_get(ch, "pn");
                        if (pjid && pjid[0]) {
                            NSString *pj = [NSString stringWithUTF8String:pjid];
                            if (pj.length) [parts addObject:pj];
                            if (ppn && ppn[0] && strstr(pjid, "@lid")) {
                                NSString *pnStr = [NSString stringWithUTF8String:ppn];
                                if (pnStr.length) [lidMappings addObject:@{ @"lid": pj, @"pn": pnStr }];
                            }
                        }
                    }
                }
                if (gJid.length) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        for (NSDictionary *mp in lidMappings) {
                            [[WAChatStore shared] registerLidMapping:mp[@"lid"] pnJid:mp[@"pn"]];
                        }
                        [[WAChatStore shared] setParticipants:parts forGroup:gJid subject:subjStr];
                    });
                }
            }
        }
    }

    char *s = wa_node_to_string(node, 0);
    NSString *xml = s ? [NSString stringWithUTF8String:s] : nil;
    free(s);
    if (!xml) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:WASessionNodeReceived object:self userInfo:@{ @"xml": xml }];
    });
}

static void cb_disconnect(void *ctx, const char *reason, int reconnect)
{
    WASession *self = (__bridge WASession *)ctx;
    NSString *r = reason ? [NSString stringWithUTF8String:reason] : @"";
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!reconnect) {
            self.state = WASessionStateDisconnected;
            [[NSNotificationCenter defaultCenter] postNotificationName:WASessionStateChanged object:self userInfo:@{ @"state": @(WASessionStateDisconnected), @"detail": r }];
        }
    });
}

static void cb_device_changed(void *ctx, wa_device *d)
{
    wa_device_save(d, [storePath() UTF8String]);
}

@implementation WASession

+ (instancetype)shared
{
    static WASession *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[WASession alloc] init]; });
    return s;
}

- (instancetype)init
{
    if ((self = [super init])) {
        if (wa_device_load(&_dev, [storePath() UTF8String]) == 0 && _dev.registered)
            self.ownJID = [NSString stringWithUTF8String:_dev.pn];
    }
    return self;
}

- (BOOL)registered { return _dev.registered != 0; }

- (void)loadOrCreateDevice
{
    const char *path = [storePath() UTF8String];
    if (wa_device_load(&_dev, path) == 0) {
        WALog(@"loaded device (registered=%d)", _dev.registered);
        if (_dev.registered) self.ownJID = [NSString stringWithUTF8String:_dev.pn];
    } else {
        WALog(@"new device");
        if (wa_device_new(&_dev) != 0) { WALog(@"key generation failed"); return; }
        wa_device_save(&_dev, path);
    }
}

- (void)start
{
    if (self.running) return;
    [self loadOrCreateDevice];
    if (!_sessions) {
        _sessions = (wa_sess_store *)malloc(sizeof *_sessions);
        if (wa_sess_load(_sessions, [sessionsPath() UTF8String]) != 0) wa_sess_init(_sessions);
    }
    memset(&_cb, 0, sizeof _cb);
    _cb.ctx = (__bridge void *)self;
    _cb.on_qr = cb_qr;
    _cb.on_paired = cb_paired;
    _cb.on_connected = cb_connected;
    _cb.on_node = cb_node;
    _cb.on_disconnect = cb_disconnect;
    _cb.on_device_changed = cb_device_changed;
    memset(&_cfg, 0, sizeof _cfg);          // defaults: web.whatsapp.com, Noise authenticates WA
    _client = wa_client_new(&_dev, &_cb, &_cfg);
    self.running = YES;
    self.state = WASessionStateConnecting;
    _thread = [[NSThread alloc] initWithTarget:self selector:@selector(run) object:nil];
    _thread.name = @"wa6-client";
    [_thread start];
}

- (void)run
{
    @autoreleasepool {
        wa_client_run(_client);
        self.running = NO;
    }
}

- (void)stop
{
    if (_client) wa_client_stop(_client);
    self.running = NO;
}

- (void)unlinkAndReset
{
    [self stop];
    [[NSFileManager defaultManager] removeItemAtPath:storePath() error:NULL];
    [[NSFileManager defaultManager] removeItemAtPath:sessionsPath() error:NULL];
    memset(&_dev, 0, sizeof _dev);
    if (_sessions) wa_sess_init(_sessions);
    self.ownJID = nil;
    self.qrString = nil;
    self.state = WASessionStateDisconnected;
}

// --- actions that run on the client thread ---

typedef struct {
    void *session;
    char jid[80];
    char text[4096];
    char mid[64];
    char quoted_id[64];
    char quoted_sender[80];
    char quoted_text[512];
    int from_me;
    int video;
} wa_action_ctx;

static void action_send_text(wa_client *c, void *ctx)
{
    wa_action_ctx *a = (wa_action_ctx *)ctx;
    WASession *self = (__bridge WASession *)a->session;
    char used_id[64] = {0};
    int rc;
    if (a->quoted_id[0]) {
        rc = wa_chat_send_ext_text(c, self->_sessions, a->jid, a->text,
                                   a->quoted_id, a->quoted_sender, a->quoted_text,
                                   a->mid[0] ? a->mid : NULL, used_id, sizeof used_id);
    } else {
        rc = wa_chat_send_text_with_id(c, self->_sessions, a->jid, a->text,
                                       a->mid[0] ? a->mid : NULL, used_id, sizeof used_id);
    }
    if (rc == 0) {
        wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
        const char *fid = a->mid[0] ? a->mid : used_id;
        if (fid[0]) {
            NSString *mid = [NSString stringWithUTF8String:fid];
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus
                                                                    object:self
                                                                  userInfo:@{ @"id": mid, @"status": @(WA_STATUS_SERVER_ACK) }];
            });
        }
    } else {
        const char *fid = a->mid[0] ? a->mid : used_id;
        if (fid[0]) {
            NSString *mid = [NSString stringWithUTF8String:fid];
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus
                                                                    object:self
                                                                  userInfo:@{ @"id": mid, @"status": @(WA_STATUS_ERROR), @"error": @"send_failed" }];
            });
        }
    }
    free(a);
}

static void action_send_reaction(wa_client *c, void *ctx)
{
    wa_action_ctx *a = (wa_action_ctx *)ctx;
    WASession *self = (__bridge WASession *)a->session;
    if (wa_chat_send_reaction(c, self->_sessions, a->jid, a->mid, a->from_me, a->text) == 0) {
        wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
    }
    free(a);
}

static void action_place_call(wa_client *c, void *ctx)
{
    wa_action_ctx *a = (wa_action_ctx *)ctx;
    WASession *self = (__bridge WASession *)a->session;
    wa_call_state st;
    if (wa_callengine_place(c, self->_sessions, a->jid, a->video, &st) == 0) {
        wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
        self->_currentCall = st;
        self->_hasCurrentCall = 1;
    }
    free(a);
}

static void action_accept_call(wa_client *c, void *ctx)
{
    WASession *self = (__bridge WASession *)ctx;
    if (self->_hasCurrentCall) {
        wa_callengine_accept(c, &self->_currentCall);
        if (self->_currentCall.has_relay) {
            start_call_media(self);
        }
    }
}

static void action_reject_call(wa_client *c, void *ctx)
{
    WASession *self = (__bridge WASession *)ctx;
    if (self->_hasCurrentCall) {
        wa_callengine_reject(c, &self->_currentCall);
        if (self->_rtc) { wa_rtc_close(self->_rtc); self->_rtc = NULL; }
        self->_hasCurrentCall = 0;
    }
}

static void action_terminate_call(wa_client *c, void *ctx)
{
    WASession *self = (__bridge WASession *)ctx;
    if (self->_hasCurrentCall) {
        wa_callengine_terminate(c, &self->_currentCall, "hangup");
        if (self->_rtc) { wa_rtc_close(self->_rtc); self->_rtc = NULL; }
        self->_hasCurrentCall = 0;
    }
}

typedef struct {
    void *session;
    char jid[80];
    char mid[40];
    char *caption;
    uint8_t *jpeg;
    size_t jpeg_len;
    uint8_t *thumb;
    size_t thumb_len;
    uint32_t duration;
} wa_image_action_ctx;

static void action_send_image(wa_client *c, void *ctx)
{
    wa_image_action_ctx *a = (wa_image_action_ctx *)ctx;
    WASession *self = (__bridge WASession *)a->session;
    char used_id[40] = { 0 };
    int rc = wa_chat_send_image(c, self->_sessions, a->jid,
                                a->jpeg, a->jpeg_len,
                                a->thumb, a->thumb_len,
                                a->caption,
                                a->mid[0] ? a->mid : NULL,
                                used_id, sizeof used_id);
    if (rc == 0) {
        wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
        const char *fid = a->mid[0] ? a->mid : used_id;
        if (fid[0]) {
            NSString *mid = [NSString stringWithUTF8String:fid];
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus
                                                                    object:self
                                                                  userInfo:@{ @"id": mid, @"status": @(WA_STATUS_SERVER_ACK) }];
            });
        }
    } else {
        const char *fid = a->mid[0] ? a->mid : used_id;
        if (fid[0]) {
            NSString *mid = [NSString stringWithUTF8String:fid];
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus
                                                                    object:self
                                                                  userInfo:@{ @"id": mid, @"status": @(WA_STATUS_ERROR), @"error": @"send_image_failed" }];
            });
        }
    }
    free(a->caption);
    free(a->jpeg);
    free(a->thumb);
    free(a);
}

static void action_send_audio(wa_client *c, void *ctx)
{
    wa_image_action_ctx *a = (wa_image_action_ctx *)ctx;
    WASession *self = (__bridge WASession *)a->session;
    char used_id[40] = { 0 };
    int rc = wa_chat_send_audio(c, self->_sessions, a->jid,
                                a->jpeg, a->jpeg_len,
                                a->duration, "audio/ogg; codecs=opus",
                                a->mid[0] ? a->mid : NULL,
                                used_id, sizeof used_id);
    if (rc == 0) {
        wa_sess_save(self->_sessions, [sessionsPath() UTF8String]);
        const char *fid = a->mid[0] ? a->mid : used_id;
        if (fid[0]) {
            NSString *mid = [NSString stringWithUTF8String:fid];
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus
                                                                    object:self
                                                                  userInfo:@{ @"id": mid, @"status": @(WA_STATUS_SERVER_ACK) }];
            });
        }
    } else {
        const char *fid = a->mid[0] ? a->mid : used_id;
        if (fid[0]) {
            NSString *mid = [NSString stringWithUTF8String:fid];
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus
                                                                    object:self
                                                                  userInfo:@{ @"id": mid, @"status": @(WA_STATUS_ERROR), @"error": @"send_audio_failed" }];
            });
        }
    }
    free(a->jpeg);
    free(a);
}

typedef struct {
    char jid[80];
    char mid[40];
    char part[80];
} wa_receipt_ctx;

static void action_send_read_receipt(wa_client *c, void *ctx)
{
    wa_receipt_ctx *r = (wa_receipt_ctx *)ctx;
    wa_node *node = wa_node_new("receipt");
    wa_node_attr(node, "to", r->jid);
    wa_node_attr(node, "id", r->mid);
    wa_node_attr(node, "type", "read");
    if (r->part[0]) wa_node_attr(node, "participant", r->part);
    wa_client_send(c, node);
    wa_node_free(node);
    free(r);
}

- (void)sendText:(NSString *)text toJID:(NSString *)jid
{
    [self sendText:text toJID:jid msgId:nil quoted:nil];
}

- (void)sendText:(NSString *)text toJID:(NSString *)jid msgId:(NSString *)mid
{
    [self sendText:text toJID:jid msgId:mid quoted:nil];
}

- (void)sendText:(NSString *)text toJID:(NSString *)jid msgId:(NSString *)mid quoted:(NSDictionary *)quoted
{
    if (!_client || !jid.length) return;
    if ([jid hasSuffix:@"@lid"]) {
        NSString *pn = [[WAChatStore shared] phoneNumberJIDForJID:jid];
        if (pn.length) jid = pn;
    }
    wa_action_ctx *a = (wa_action_ctx *)calloc(1, sizeof *a);
    a->session = (__bridge void *)self;
    snprintf(a->jid, sizeof a->jid, "%s", jid.UTF8String);
    snprintf(a->text, sizeof a->text, "%s", text.UTF8String ?: "");
    if (mid.length) snprintf(a->mid, sizeof a->mid, "%s", mid.UTF8String);
    if ([quoted isKindOfClass:[NSDictionary class]]) {
        NSString *qid = quoted[@"id"];
        NSString *qtxt = quoted[@"text"];
        if ([qid isKindOfClass:[NSString class]] && qid.length) {
            snprintf(a->quoted_id, sizeof a->quoted_id, "%s", qid.UTF8String);
            if ([qtxt isKindOfClass:[NSString class]] && qtxt.length)
                snprintf(a->quoted_text, sizeof a->quoted_text, "%s", qtxt.UTF8String);
        }
    }
    if (wa_client_post(_client, action_send_text, a) != 0) {
        if (mid.length) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus
                                                                    object:self
                                                                  userInfo:@{ @"id": mid, @"status": @(WA_STATUS_ERROR), @"error": @"post_failed" }];
            });
        }
        free(a);
    }
}

- (void)sendReaction:(NSString *)emoji toMessageId:(NSString *)mid fromMe:(BOOL)fromMe inChat:(NSString *)jid
{
    if (!_client || !jid.length || !mid.length) return;
    if ([jid hasSuffix:@"@lid"]) {
        NSString *pn = [[WAChatStore shared] phoneNumberJIDForJID:jid];
        if (pn.length) jid = pn;
    }
    wa_action_ctx *a = (wa_action_ctx *)calloc(1, sizeof *a);
    a->session = (__bridge void *)self;
    snprintf(a->jid, sizeof a->jid, "%s", jid.UTF8String);
    snprintf(a->mid, sizeof a->mid, "%s", mid.UTF8String);
    snprintf(a->text, sizeof a->text, "%s", emoji.UTF8String ?: "");
    a->from_me = fromMe ? 1 : 0;
    if (wa_client_post(_client, action_send_reaction, a) != 0) free(a);
}

- (void)sendImage:(NSData *)jpegData thumb:(NSData *)thumbData caption:(NSString *)caption toJID:(NSString *)jid msgId:(NSString *)mid
{
    if (!_client || !jid.length || !jpegData.length) return;
    if ([jid hasSuffix:@"@lid"]) {
        NSString *pn = [[WAChatStore shared] phoneNumberJIDForJID:jid];
        if (pn.length) jid = pn;
    }
    wa_image_action_ctx *a = (wa_image_action_ctx *)calloc(1, sizeof *a);
    a->session = (__bridge void *)self;
    snprintf(a->jid, sizeof a->jid, "%s", jid.UTF8String);
    if (mid.length) snprintf(a->mid, sizeof a->mid, "%s", mid.UTF8String);
    if (caption.length) a->caption = strdup(caption.UTF8String);
    a->jpeg = (uint8_t *)malloc(jpegData.length);
    if (a->jpeg) {
        memcpy(a->jpeg, jpegData.bytes, jpegData.length);
        a->jpeg_len = jpegData.length;
    }
    if (thumbData.length) {
        a->thumb = (uint8_t *)malloc(thumbData.length);
        if (a->thumb) {
            memcpy(a->thumb, thumbData.bytes, thumbData.length);
            a->thumb_len = thumbData.length;
        }
    }
    if (wa_client_post(_client, action_send_image, a) != 0) {
        if (mid.length) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus
                                                                    object:self
                                                                  userInfo:@{ @"id": mid, @"status": @(WA_STATUS_ERROR), @"error": @"post_failed" }];
            });
        }
        free(a->caption);
        free(a->jpeg);
        free(a->thumb);
        free(a);
    }
}

- (void)sendAudio:(NSData *)audioData duration:(uint32_t)dur toJID:(NSString *)jid msgId:(NSString *)mid
{
    if (!_client || !jid.length || !audioData.length) return;
    if ([jid hasSuffix:@"@lid"]) {
        NSString *pn = [[WAChatStore shared] phoneNumberJIDForJID:jid];
        if (pn.length) jid = pn;
    }
    wa_image_action_ctx *a = (wa_image_action_ctx *)calloc(1, sizeof *a);
    a->session = (__bridge void *)self;
    snprintf(a->jid, sizeof a->jid, "%s", jid.UTF8String);
    if (mid.length) snprintf(a->mid, sizeof a->mid, "%s", mid.UTF8String);
    a->duration = dur;
    a->jpeg = (uint8_t *)malloc(audioData.length);
    if (a->jpeg) {
        memcpy(a->jpeg, audioData.bytes, audioData.length);
        a->jpeg_len = audioData.length;
    }
    if (wa_client_post(_client, action_send_audio, a) != 0) {
        if (mid.length) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:WASessionMessageStatus
                                                                    object:self
                                                                  userInfo:@{ @"id": mid, @"status": @(WA_STATUS_ERROR), @"error": @"post_failed" }];
            });
        }
        free(a->jpeg);
        free(a);
    }
}

- (void)sendReadReceiptForMessage:(NSString *)mid toJID:(NSString *)jid participant:(NSString *)part
{
    if (!_client || !mid.length || !jid.length) return;
    wa_receipt_ctx *r = (wa_receipt_ctx *)calloc(1, sizeof *r);
    snprintf(r->jid, sizeof r->jid, "%s", jid.UTF8String);
    snprintf(r->mid, sizeof r->mid, "%s", mid.UTF8String);
    if (part.length) snprintf(r->part, sizeof r->part, "%s", part.UTF8String);
    if (wa_client_post(_client, action_send_read_receipt, r) != 0) free(r);
}

- (void)placeCallTo:(NSString *)jid video:(BOOL)video
{
    if (!_client || !jid.length) return;
    NSString *callJid = jid;
    if ([callJid hasSuffix:@"@lid"]) {
        NSString *pn = [[WAChatStore shared] phoneNumberJIDForJID:callJid];
        if (pn.length) callJid = pn;
    }
    [[WAChatStore shared] recordCallLog:@{
        @"jid": jid,
        @"name": WADisplayNameForChat(jid, nil),
        @"out": @YES,
        @"video": @(video),
        @"ts": @((unsigned long long)[[NSDate date] timeIntervalSince1970])
    }];
    wa_action_ctx *a = (wa_action_ctx *)calloc(1, sizeof *a);
    a->session = (__bridge void *)self;
    snprintf(a->jid, sizeof a->jid, "%s", callJid.UTF8String);
    a->video = video ? 1 : 0;
    if (wa_client_post(_client, action_place_call, a) != 0) free(a);
}

- (void)acceptCall
{
    if (!_client) return;
    wa_client_post(_client, action_accept_call, (__bridge void *)self);
}

- (void)rejectCall
{
    if (!_client) return;
    wa_client_post(_client, action_reject_call, (__bridge void *)self);
}

- (void)terminateCall
{
    if (!_client) return;
    wa_client_post(_client, action_terminate_call, (__bridge void *)self);
}

- (void)sendCallAudioFrame:(NSData *)opusFrame
{
    if (!_rtc || !_currentCall.srtp_ready || opusFrame.length == 0 || opusFrame.length > 1200) return;

    uint8_t rtp[1500];
    rtp[0] = 0x80;
    rtp[1] = 111; // Opus dynamic payload type
    uint16_t seq = _currentCall.rtp_seq++;
    wa_put_u16(rtp + 2, seq);
    wa_put_u32(rtp + 4, _currentCall.rtp_ts);
    _currentCall.rtp_ts += 320;
    wa_put_u32(rtp + 8, _currentCall.send_ssrc);

    memcpy(rtp + 12, opusFrame.bytes, opusFrame.length);
    uint32_t roc = wa_roc_send_advance(&_currentCall.roc_send, seq);
    wa_srtp_crypt_payload(&_currentCall.send_keys, _currentCall.send_ssrc, roc, seq, rtp + 12, opusFrame.length);

    size_t tag_len = _currentCall.relay.warp_mi_tag_len ? _currentCall.relay.warp_mi_tag_len : WA_WARP_TAG_LEN;
    wa_srtp_warp_tag(_currentCall.send_keys.auth_key, rtp, 12 + opusFrame.length, roc, rtp + 12 + opusFrame.length);

    wa_rtc_send(_rtc, rtp, 12 + opusFrame.length + tag_len);
}

typedef struct {
    char jid[80];
    char state[64];
} wa_presence_ctx;

static void action_send_presence(wa_client *c, void *ctx)
{
    wa_presence_ctx *p = (wa_presence_ctx *)ctx;
    wa_node *n = wa_node_new("presence");
    wa_node_attr(n, "type", p->state);
    wa_node_attr(n, "to", p->jid);
    wa_client_send(c, n);
    wa_node_free(n);
    free(p);
}

static void action_send_chatstate(wa_client *c, void *ctx)
{
    wa_presence_ctx *p = (wa_presence_ctx *)ctx;
    wa_node *n = wa_node_new("chatstate");
    wa_node_attr(n, "to", p->jid);
    wa_node_add_new(n, p->state);
    wa_client_send(c, n);
    wa_node_free(n);
    free(p);
}

- (void)subscribePresence:(NSString *)jid
{
    if (!_client || !jid.length) return;
    if ([jid hasSuffix:@"@lid"]) {
        NSString *pn = [[WAChatStore shared] phoneNumberJIDForJID:jid];
        if (pn.length) jid = pn;
    }
    wa_presence_ctx *p = (wa_presence_ctx *)calloc(1, sizeof *p);
    snprintf(p->jid, sizeof p->jid, "%s", jid.UTF8String);
    snprintf(p->state, sizeof p->state, "subscribe");
    if (wa_client_post(_client, action_send_presence, p) != 0) free(p);
}

- (void)unsubscribePresence:(NSString *)jid
{
    if (!_client || !jid.length) return;
    if ([jid hasSuffix:@"@lid"]) {
        NSString *pn = [[WAChatStore shared] phoneNumberJIDForJID:jid];
        if (pn.length) jid = pn;
    }
    wa_presence_ctx *p = (wa_presence_ctx *)calloc(1, sizeof *p);
    snprintf(p->jid, sizeof p->jid, "%s", jid.UTF8String);
    snprintf(p->state, sizeof p->state, "unsubscribe");
    if (wa_client_post(_client, action_send_presence, p) != 0) free(p);
}

- (void)sendChatState:(BOOL)composing toJID:(NSString *)jid
{
    if (!_client || !jid.length) return;
    if ([jid hasSuffix:@"@lid"]) {
        NSString *pn = [[WAChatStore shared] phoneNumberJIDForJID:jid];
        if (pn.length) jid = pn;
    }
    wa_presence_ctx *p = (wa_presence_ctx *)calloc(1, sizeof *p);
    snprintf(p->jid, sizeof p->jid, "%s", jid.UTF8String);
    snprintf(p->state, sizeof p->state, "%s", composing ? "composing" : "paused");
    if (wa_client_post(_client, action_send_chatstate, p) != 0) free(p);
}

static void action_send_recording_state(wa_client *c, void *ctx)
{
    wa_presence_ctx *p = (wa_presence_ctx *)ctx;
    wa_node *n = wa_node_new("chatstate");
    wa_node_attr(n, "to", p->jid);
    if (strcmp(p->state, "recording") == 0) {
        wa_node *comp = wa_node_add_new(n, "composing");
        wa_node_attr(comp, "media", "audio");
    } else {
        wa_node_add_new(n, "paused");
    }
    wa_client_send(c, n);
    wa_node_free(n);
    free(p);
}

- (void)sendRecordingState:(BOOL)recording toJID:(NSString *)jid
{
    if (!_client || !jid.length) return;
    if ([jid hasSuffix:@"@lid"]) {
        NSString *pn = [[WAChatStore shared] phoneNumberJIDForJID:jid];
        if (pn.length) jid = pn;
    }
    wa_presence_ctx *p = (wa_presence_ctx *)calloc(1, sizeof *p);
    snprintf(p->jid, sizeof p->jid, "%s", jid.UTF8String);
    snprintf(p->state, sizeof p->state, "%s", recording ? "recording" : "paused");
    if (wa_client_post(_client, action_send_recording_state, p) != 0) free(p);
}

static void action_set_block(wa_client *c, void *ctx)
{
    wa_presence_ctx *p = (wa_presence_ctx *)ctx;
    char bare[80] = {0};
    wa_jid_to_non_ad(p->jid, bare, sizeof bare);
    wa_node *iq = wa_node_new("iq");
    char idbuf[32];
    snprintf(idbuf, sizeof idbuf, "blk_%08x", (unsigned int)arc4random());
    wa_node_attr(iq, "id", idbuf);
    wa_node_attr(iq, "to", "@s.whatsapp.net");
    wa_node_attr(iq, "type", "set");
    wa_node_attr(iq, "xmlns", "blocklist");
    wa_node *item = wa_node_add_new(iq, "item");
    wa_node_attr(item, "jid", bare[0] ? bare : p->jid);
    wa_node_attr(item, "action", p->state);
    wa_client_send(c, iq);
    wa_node_free(iq);
    free(p);
}

- (void)setBlockContact:(NSString *)jid blocked:(BOOL)blocked
{
    if (!_client || !jid.length) return;
    wa_presence_ctx *p = (wa_presence_ctx *)calloc(1, sizeof *p);
    snprintf(p->jid, sizeof p->jid, "%s", jid.UTF8String);
    snprintf(p->state, sizeof p->state, "%s", blocked ? "block" : "unblock");
    if (wa_client_post(_client, action_set_block, p) != 0) free(p);
}

static void action_fetch_picture(wa_client *c, void *ctx)
{
    wa_presence_ctx *p = (wa_presence_ctx *)ctx;
    char bare[80] = {0};
    wa_jid_to_non_ad(p->jid, bare, sizeof bare);
    wa_node *iq = wa_node_new("iq");
    wa_node_attr(iq, "id", p->state[0] ? p->state : "pic_req");
    wa_node_attr(iq, "to", "@s.whatsapp.net");
    wa_node_attr(iq, "target", bare[0] ? bare : p->jid);
    wa_node_attr(iq, "type", "get");
    wa_node_attr(iq, "xmlns", "w:profile:picture");
    wa_node *pic = wa_node_add_new(iq, "picture");
    wa_node_attr(pic, "type", "preview");
    wa_node_attr(pic, "query", "url");
    wa_client_send(c, iq);
    wa_node_free(iq);
    free(p);
}

- (void)fetchProfilePicture:(NSString *)jid
{
    if (!_client || !jid.length || [jid hasSuffix:@"@broadcast"]) return;
    NSString *key = [WAChatStore keyForJID:jid];
    if (!key.length) return;

    wa_presence_ctx *p = (wa_presence_ctx *)calloc(1, sizeof *p);
    snprintf(p->jid, sizeof p->jid, "%s", jid.UTF8String);
    snprintf(p->state, sizeof p->state, "pic_%s", key.UTF8String);
    if (wa_client_post(_client, action_fetch_picture, p) != 0) free(p);

    // If jid is @lid and we also know its @s.whatsapp.net phone JID, query that too so either yields the avatar
    if ([jid hasSuffix:@"@lid"]) {
        NSString *pnJid = [[WAChatStore shared] phoneNumberJIDForJID:jid];
        if (pnJid.length) {
            wa_presence_ctx *p2 = (wa_presence_ctx *)calloc(1, sizeof *p2);
            snprintf(p2->jid, sizeof p2->jid, "%s", pnJid.UTF8String);
            snprintf(p2->state, sizeof p2->state, "pic_%s_pn", key.UTF8String);
            if (wa_client_post(_client, action_fetch_picture, p2) != 0) free(p2);
        }
    }
}

static void action_fetch_group_metadata(wa_client *c, void *ctx)
{
    wa_presence_ctx *p = (wa_presence_ctx *)ctx;
    wa_node *iq = wa_node_new("iq");
    wa_node_attr(iq, "id", p->state[0] ? p->state : "grp_req");
    wa_node_attr(iq, "to", p->jid);
    wa_node_attr(iq, "type", "get");
    wa_node_attr(iq, "xmlns", "w:g2");
    wa_node *q = wa_node_add_new(iq, "query");
    wa_node_attr(q, "request", "interactive");
    wa_client_send(c, iq);
    wa_node_free(iq);
    free(p);
}

- (void)fetchGroupMetadata:(NSString *)groupJID
{
    if (!_client || !groupJID.length || ![groupJID hasSuffix:@"@g.us"]) return;
    NSString *key = [WAChatStore keyForJID:groupJID];
    wa_presence_ctx *p = (wa_presence_ctx *)calloc(1, sizeof *p);
    snprintf(p->jid, sizeof p->jid, "%s", groupJID.UTF8String);
    snprintf(p->state, sizeof p->state, "grp_%s", key.UTF8String);
    if (wa_client_post(_client, action_fetch_group_metadata, p) != 0) free(p);
}

- (void)dealloc { if (_sessions) free(_sessions); }

@end
