#include "wa_history.h"
#include "wa_proto.h"
#include "wa_crypto.h"
#include "wa_net.h"
#include "wa_message.h"

#include <string.h>
#include <stdlib.h>
#include <zlib.h>

// -------- proto field numbers (from the WhatsApp schema; see waproto/whatsapp.proto in the refs) --------
// Message:                      conversation=1, extendedTextMessage=6, protocolMessage=12
// ProtocolMessage:             historySyncNotification=6
// HistorySyncNotification:     fileLength=2, mediaKey=3, fileEncSha256=4, directPath=5, syncType=6,
//                              chunkOrder=7, initialHistBootstrapInlinePayload=11
// HistorySync:                 syncType=1, conversations=2 (repeated Conversation)
// Conversation:                id=1, messages=2 (repeated HistorySyncMsg), lastMsgTimestamp=5, unreadCount=6,
//                              conversationTimestamp=12, name=13, displayName=38
// HistorySyncMsg:              message=1 (WebMessageInfo)
// WebMessageInfo:              key=1, message=2 (Message), messageTimestamp=3
#define F_MSG_PROTOCOLMESSAGE      12
#define F_PROTO_HISTORYSYNCNOTIF    6
#define F_HSN_FILELENGTH            2
#define F_HSN_MEDIAKEY              3
#define F_HSN_FILEENCSHA256         4
#define F_HSN_DIRECTPATH            5
#define F_HSN_SYNCTYPE              6
#define F_HSN_INLINEPAYLOAD        11
#define F_HS_SYNCTYPE               1
#define F_HS_CONVERSATIONS          2
#define F_HS_PUSHNAMES              7
#define F_HS_PN_LID_MAPPINGS       15
#define F_PUSHNAME_ID               1
#define F_PUSHNAME_NAME             2
#define F_MAPPING_PNJID             1
#define F_MAPPING_LIDJID            2
#define F_CONV_ID                   1
#define F_CONV_MESSAGES             2
#define F_CONV_LASTMSGTS            5
#define F_CONV_UNREAD               6
#define F_CONV_TIMESTAMP           12
#define F_CONV_NAME                13
#define F_CONV_NEWJID              29
#define F_CONV_DISPLAYNAME         38
#define F_CONV_LIDJID              40
#define F_CONV_PNJID               42
#define F_HSM_MESSAGE               1
#define F_WMI_KEY                    1
#define F_WMI_MESSAGE               2
#define F_WMI_MESSAGETIMESTAMP      3
#define F_WMI_STATUS                4
#define F_KEY_FROMME                2
#define F_KEY_ID                    3
// Message media sub-message fields:
#define F_MSG_CONTACT               4
#define F_MSG_LOCATION              5
#define F_MSG_IMAGE                 3
#define F_MSG_DOCUMENT              7
#define F_MSG_AUDIO                 8
#define F_MSG_VIDEO                 9
#define F_MSG_STICKER              26
// ImageMessage: caption=3, jpegThumbnail=16. VideoMessage: seconds=5, caption=7, jpegThumbnail=16.
// AudioMessage: seconds=5. DocumentMessage: title=3, fileName=8, caption=20, jpegThumbnail=16. ContactMessage: displayName=1.
#define F_IMG_CAPTION               3
#define F_JPEG_THUMBNAIL           16
#define F_VID_SECONDS               5
#define F_VID_CAPTION               7
#define F_AUD_SECONDS               5
#define F_DOC_TITLE                 3
#define F_DOC_FILENAME              8
#define F_DOC_CAPTION              20
#define F_CONTACT_DISPLAYNAME       1
#define F_WMI_PARTICIPANT           5
#define F_WMI_PUSHNAME             19
#define F_KEY_PARTICIPANT           4
// per-type media download refs (mediaKey, fileEncSha256, directPath):
#define F_IMG_MEDIAKEY              8
#define F_IMG_ENCSHA                9
#define F_IMG_DIRECTPATH           11
#define F_VID_MEDIAKEY              6
#define F_VID_ENCSHA               11
#define F_VID_DIRECTPATH           13
#define F_AUD_MEDIAKEY              7
#define F_AUD_ENCSHA                8
#define F_AUD_DIRECTPATH            9
#define F_DOC_MEDIAKEY              7
#define F_DOC_ENCSHA                9
#define F_DOC_DIRECTPATH           10

#define HISTORY_MEDIA_HOST   "mmg.whatsapp.net"
#define HISTORY_HKDF_INFO    "WhatsApp History Keys"
#define MEDIA_MAC_SIZE       10

static void conv_free(wa_history_conv *c)
{
    free(c->jid);
    free(c->name);
    free(c->last);
    free(c->pn_jid);
    free(c->lid_jid);
    for (int i = 0; i < c->nmsgs; i++) {
        free(c->msgs[i].text);
        free(c->msgs[i].id);
        free(c->msgs[i].thumb);
        free(c->msgs[i].filename);
        free(c->msgs[i].sender);
        free(c->msgs[i].direct_path);
        free(c->msgs[i].media_key);
        free(c->msgs[i].enc_sha);
        free(c->msgs[i].quoted_id);
        free(c->msgs[i].quoted_text);
    }
    free(c->msgs);
}

// Extract a media message's download refs (mediaKey/fileEncSha256/directPath) at the given field numbers.
static void copy_dl_refs(const uint8_t *sub, size_t subl, uint32_t mk, uint32_t enc, uint32_t dp, wa_history_msg *m)
{
    const uint8_t *d;
    size_t n;
    if (pb_get_bytes(sub, subl, mk, &d, &n) && n == 32) {
        m->media_key = (uint8_t *)malloc(32);
        if (m->media_key) { memcpy(m->media_key, d, 32); m->media_key_len = 32; }
    }
    if (pb_get_bytes(sub, subl, enc, &d, &n) && n > 0) {
        m->enc_sha = (uint8_t *)malloc(n);
        if (m->enc_sha) { memcpy(m->enc_sha, d, n); m->enc_sha_len = n; }
    }
    m->direct_path = pb_get_string(sub, subl, dp);
}

// Copy a bytes field (e.g. jpegThumbnail) out of a sub-message into m->thumb.
static void copy_thumb(const uint8_t *sub, size_t subl, uint32_t field, wa_history_msg *m)
{
    const uint8_t *d;
    size_t n;
    if (pb_get_bytes(sub, subl, field, &d, &n) && n > 0) {
        m->thumb = (uint8_t *)malloc(n);
        if (m->thumb) { memcpy(m->thumb, d, n); m->thumb_len = n; }
    }
}

static void unwrap_inner_msg(const uint8_t **msg, size_t *len)
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
        if (pb_get_bytes(*msg, *len, 12, &wrap, &wl) && pb_get_bytes(wrap, wl, 14, &inner, &il)) {
            *msg = inner; *len = il; continue; // protocolMessage (12) -> editedMessage (14)
        }
        break;
    }
}

// Fill m->{kind,text,thumb,duration,filename} from a decoded Message. Returns 1 for a displayable message, 0 to skip
// (protocol/system messages, reactions, key distribution, …). Text and the recognised media types are displayable.
static int extract_msg_content(const uint8_t *inner, size_t innerl, wa_history_msg *m)
{
    unwrap_inner_msg(&inner, &innerl);

    char qid[96] = {0}, qtxt[256] = {0};
    if (wa_msg_parse_quoted(inner, innerl, qid, sizeof qid, qtxt, sizeof qtxt) && qid[0]) {
        m->quoted_id = wa_strdup(qid);
        if (qtxt[0]) m->quoted_text = wa_strdup(qtxt);
    }

    char *text = wa_msg_parse_text(inner, innerl);
    if (text) { m->kind = WA_MSG_TEXT; m->text = text; return 1; }

    const uint8_t *sub;
    size_t subl;
    uint64_t v;
    if (pb_get_bytes(inner, innerl, F_MSG_IMAGE, &sub, &subl)) {
        m->kind = WA_MSG_IMAGE;
        m->text = pb_get_string(sub, subl, F_IMG_CAPTION);
        copy_thumb(sub, subl, F_JPEG_THUMBNAIL, m);
        copy_dl_refs(sub, subl, F_IMG_MEDIAKEY, F_IMG_ENCSHA, F_IMG_DIRECTPATH, m);
        return 1;
    }
    if (pb_get_bytes(inner, innerl, F_MSG_VIDEO, &sub, &subl)) {
        m->kind = WA_MSG_VIDEO;
        m->text = pb_get_string(sub, subl, F_VID_CAPTION);
        copy_thumb(sub, subl, F_JPEG_THUMBNAIL, m);
        copy_dl_refs(sub, subl, F_VID_MEDIAKEY, F_VID_ENCSHA, F_VID_DIRECTPATH, m);
        if (pb_get_varint(sub, subl, F_VID_SECONDS, &v)) m->duration = (uint32_t)v;
        return 1;
    }
    if (pb_get_bytes(inner, innerl, F_MSG_AUDIO, &sub, &subl)) {
        m->kind = WA_MSG_AUDIO;
        copy_dl_refs(sub, subl, F_AUD_MEDIAKEY, F_AUD_ENCSHA, F_AUD_DIRECTPATH, m);
        if (pb_get_varint(sub, subl, F_AUD_SECONDS, &v)) m->duration = (uint32_t)v;
        return 1;
    }
    if (pb_get_bytes(inner, innerl, F_MSG_DOCUMENT, &sub, &subl)) {
        m->kind = WA_MSG_DOCUMENT;
        m->filename = pb_get_string(sub, subl, F_DOC_FILENAME);
        if (!m->filename) m->filename = pb_get_string(sub, subl, F_DOC_TITLE);
        m->text = pb_get_string(sub, subl, F_DOC_CAPTION);
        copy_thumb(sub, subl, F_JPEG_THUMBNAIL, m);
        copy_dl_refs(sub, subl, F_DOC_MEDIAKEY, F_DOC_ENCSHA, F_DOC_DIRECTPATH, m);
        return 1;
    }
    if (pb_get_bytes(inner, innerl, F_MSG_STICKER, &sub, &subl)) { m->kind = WA_MSG_STICKER; return 1; }
    if (pb_get_bytes(inner, innerl, F_MSG_LOCATION, &sub, &subl)) { m->kind = WA_MSG_LOCATION; return 1; }
    if (pb_get_bytes(inner, innerl, F_MSG_CONTACT, &sub, &subl)) {
        m->kind = WA_MSG_CONTACT;
        m->text = pb_get_string(sub, subl, F_CONTACT_DISPLAYNAME);
        return 1;
    }
    free(m->quoted_id); m->quoted_id = NULL;
    free(m->quoted_text); m->quoted_text = NULL;
    return 0;
}

int wa_history_msg_parse(const uint8_t *inner, size_t innerl, wa_history_msg *m)
{
    return extract_msg_content(inner, innerl, m);
}

void wa_history_msg_free(wa_history_msg *m)
{
    if (!m) return;
    free(m->text);
    free(m->id);
    free(m->thumb);
    free(m->filename);
    free(m->sender);
    free(m->direct_path);
    free(m->media_key);
    free(m->enc_sha);
    free(m->quoted_id);
    free(m->quoted_text);
    memset(m, 0, sizeof *m);
}

static int msg_cmp_ts(const void *a, const void *b)
{
    const wa_history_msg *x = (const wa_history_msg *)a, *y = (const wa_history_msg *)b;
    if (x->timestamp < y->timestamp) return -1;
    if (x->timestamp > y->timestamp) return 1;
    return 0;
}

void wa_history_result_free(wa_history_result *r)
{
    if (!r) return;
    for (int i = 0; i < r->nconvs; i++) conv_free(&r->convs[i]);
    free(r->convs);
    r->convs = NULL;
    r->nconvs = 0;
    for (int i = 0; i < r->npushnames; i++) {
        free(r->pushnames[i].id);
        free(r->pushnames[i].pushname);
    }
    free(r->pushnames);
    r->pushnames = NULL;
    r->npushnames = 0;
    for (int i = 0; i < r->nmappings; i++) {
        free(r->mappings[i].pn_jid);
        free(r->mappings[i].lid_jid);
    }
    free(r->mappings);
    r->mappings = NULL;
    r->nmappings = 0;
}

// Parse one Conversation message into out. Returns 0 (out filled, caller owns the strings) or -1 (skip this one).
static int parse_conversation(const uint8_t *c, size_t cl, wa_history_conv *out)
{
    memset(out, 0, sizeof *out);
    char *id = pb_get_string(c, cl, F_CONV_ID);
    if (!id || !id[0]) { free(id); return -1; }
    out->jid = id;
    out->name = pb_get_string(c, cl, F_CONV_NAME);
    if (!out->name) out->name = pb_get_string(c, cl, F_CONV_DISPLAYNAME);
    out->pn_jid = pb_get_string(c, cl, F_CONV_PNJID);
    if (!out->pn_jid) out->pn_jid = pb_get_string(c, cl, F_CONV_NEWJID);
    out->lid_jid = pb_get_string(c, cl, F_CONV_LIDJID);

    uint64_t ts = 0;
    if (pb_get_varint(c, cl, F_CONV_TIMESTAMP, &ts)) out->timestamp = ts;
    else if (pb_get_varint(c, cl, F_CONV_LASTMSGTS, &ts)) out->timestamp = ts;
    uint64_t unread = 0;
    if (pb_get_varint(c, cl, F_CONV_UNREAD, &unread)) out->unread = (uint32_t)unread;

    // Collect the messages: scan the repeated messages (HistorySyncMsg), extracting each WebMessageInfo's content
    // (text or a recognised media kind), timestamp and fromMe. Protocol/system messages are skipped.
    wa_history_msg *msgs = NULL;
    int n = 0, mcap = 0;
    pb_reader r;
    pb_reader_init(&r, c, cl);
    pb_field f;
    int rc;
    while ((rc = pb_next(&r, &f)) > 0) {
        if (f.field != F_CONV_MESSAGES || f.wire != PB_BYTES) continue;
        const uint8_t *wmi;
        size_t wmil;
        if (!pb_get_bytes(f.data, f.len, F_HSM_MESSAGE, &wmi, &wmil)) continue;   // HistorySyncMsg.message -> WebMessageInfo
        const uint8_t *inner;
        size_t innerl;
        if (!pb_get_bytes(wmi, wmil, F_WMI_MESSAGE, &inner, &innerl)) continue;
        wa_history_msg msg;
        memset(&msg, 0, sizeof msg);
        if (!extract_msg_content(inner, innerl, &msg)) continue;   // not a displayable message
        pb_get_varint(wmi, wmil, F_WMI_MESSAGETIMESTAMP, &msg.timestamp);
        uint64_t msg_st = 0;
        if (pb_get_varint(wmi, wmil, F_WMI_STATUS, &msg_st)) msg.status = (int)msg_st;
        const uint8_t *key;
        size_t keyl;
        if (pb_get_bytes(wmi, wmil, F_WMI_KEY, &key, &keyl)) {
            uint64_t fm = 0;
            if (pb_get_varint(key, keyl, F_KEY_FROMME, &fm)) msg.from_me = fm ? 1 : 0;
            msg.id = pb_get_string(key, keyl, F_KEY_ID);
            if (!msg.from_me) {
                char *push = pb_get_string(wmi, wmil, F_WMI_PUSHNAME);
                if (push) {
                    msg.sender = push;
                    if (!out->name && !strstr(out->jid, "@g.us")) {
                        out->name = strdup(push);
                    }
                } else {
                    char *part = pb_get_string(wmi, wmil, F_WMI_PARTICIPANT);
                    if (!part) part = pb_get_string(key, keyl, F_KEY_PARTICIPANT);
                    if (part) { char *at = strchr(part, '@'); if (at) *at = 0; msg.sender = part; }
                }
            }
        }
        if (n >= mcap) {
            int ncap = mcap ? mcap * 2 : 16;
            wa_history_msg *p = (wa_history_msg *)realloc(msgs, (size_t)ncap * sizeof *p);
            if (!p) { wa_history_msg_free(&msg); break; }
            msgs = p;
            mcap = ncap;
        }
        msgs[n++] = msg;
        if (n >= 4096) break;   // defensive per-conversation cap
    }
    if (n > 1) qsort(msgs, (size_t)n, sizeof *msgs, msg_cmp_ts);   // oldest-first for display
    out->msgs = msgs;
    out->nmsgs = n;
    if (n > 0) {
        if (msgs[n - 1].text) out->last = wa_strdup(msgs[n - 1].text);   // may be NULL for captionless media
        if (out->timestamp == 0) out->timestamp = msgs[n - 1].timestamp;
    }
    return 0;
}

int wa_history_parse(const uint8_t *data, size_t len, wa_history_result *out)
{
    memset(out, 0, sizeof *out);
    uint64_t st = 0;
    if (pb_get_varint(data, len, F_HS_SYNCTYPE, &st)) out->sync_type = (int)st;

    pb_reader r;
    pb_reader_init(&r, data, len);
    pb_field f;
    int cap = 0, pncap = 0, mapcap = 0, rc;
    while ((rc = pb_next(&r, &f)) > 0) {
        if (f.field == F_HS_CONVERSATIONS && f.wire == PB_BYTES) {
            wa_history_conv conv;
            if (parse_conversation(f.data, f.len, &conv) != 0) continue;
            if (out->nconvs >= cap) {
                int ncap = cap ? cap * 2 : 16;
                wa_history_conv *p = (wa_history_conv *)realloc(out->convs, (size_t)ncap * sizeof *p);
                if (!p) { conv_free(&conv); break; }
                out->convs = p;
                cap = ncap;
            }
            out->convs[out->nconvs++] = conv;
            if (out->nconvs >= 4096) break;   // defensive cap
        } else if (f.field == F_HS_PUSHNAMES && f.wire == PB_BYTES) {
            char *pid = pb_get_string(f.data, f.len, F_PUSHNAME_ID);
            char *pname = pb_get_string(f.data, f.len, F_PUSHNAME_NAME);
            if (!pname) pname = pb_get_string(f.data, f.len, 3);
            if (pid && pname && pname[0]) {
                if (out->npushnames >= pncap) {
                    int npcap = pncap ? pncap * 2 : 32;
                    wa_history_pushname *np = (wa_history_pushname *)realloc(out->pushnames, (size_t)npcap * sizeof *np);
                    if (!np) { free(pid); free(pname); break; }
                    out->pushnames = np;
                    pncap = npcap;
                }
                out->pushnames[out->npushnames].id = pid;
                out->pushnames[out->npushnames].pushname = pname;
                out->npushnames++;
            } else { free(pid); free(pname); }
        } else if (f.field == F_HS_PN_LID_MAPPINGS && f.wire == PB_BYTES) {
            char *pn = pb_get_string(f.data, f.len, F_MAPPING_PNJID);
            char *lid = pb_get_string(f.data, f.len, F_MAPPING_LIDJID);
            if (pn && lid && pn[0] && lid[0]) {
                if (out->nmappings >= mapcap) {
                    int nmcap = mapcap ? mapcap * 2 : 32;
                    wa_history_lid_mapping *nm = (wa_history_lid_mapping *)realloc(out->mappings, (size_t)nmcap * sizeof *nm);
                    if (!nm) { free(pn); free(lid); break; }
                    out->mappings = nm;
                    mapcap = nmcap;
                }
                out->mappings[out->nmappings].pn_jid = pn;
                out->mappings[out->nmappings].lid_jid = lid;
                out->nmappings++;
            } else { free(pn); free(lid); }
        }
    }
    if (rc < 0) wa_log("history: malformed HistorySync (parsed %d conversations before the error)", out->nconvs);
    return 0;
}

int wa_history_parse_compressed(const uint8_t *in, size_t len, wa_history_result *out)
{
    memset(out, 0, sizeof *out);
    wa_buf plain;
    wa_buf_init(&plain);
    if (wa_zlib_inflate(in, len, &plain) != 0) { wa_buf_free(&plain); wa_log("history: inflate failed"); return -1; }
    int rc = wa_history_parse(plain.data, plain.len, out);
    wa_buf_free(&plain);
    return rc;
}

// Generic WhatsApp media decrypt: HKDF(media_key, info) -> iv/cipher/mac, verify the trailing 10-byte HMAC over
// iv||ciphertext, AES-256-CBC. Appends plaintext to out. 0 or -1.
static int media_decrypt_info(const uint8_t *blob, size_t blob_len, const uint8_t *media_key, const char *info, wa_buf *out)
{
    if (blob_len <= MEDIA_MAC_SIZE) return -1;
    uint8_t expanded[112];
    if (wa_hkdf_sha256(NULL, 0, media_key, 32, info, strlen(info), expanded, sizeof expanded) != 0) return -1;
    const uint8_t *iv = expanded, *cipher_key = expanded + 16, *mac_key = expanded + 48;
    size_t ct_len = blob_len - MEDIA_MAC_SIZE;
    const void *parts[2] = { iv, blob };
    size_t lens[2] = { 16, ct_len };
    uint8_t mac[32];
    wa_hmac_sha256_v(mac_key, 32, 2, parts, lens, mac);
    if (wa_ct_compare(mac, blob + ct_len, MEDIA_MAC_SIZE) != 0) { wa_log("media: MAC mismatch"); return -1; }
    return wa_aes_cbc_decrypt(cipher_key, iv, blob, ct_len, out);
}

int wa_history_media_decrypt(const uint8_t *blob, size_t blob_len, const uint8_t media_key[32], wa_buf *out)
{
    return media_decrypt_info(blob, blob_len, media_key, HISTORY_HKDF_INFO, out);
}

const char *wa_media_app_info(int kind)
{
    switch (kind) {
        case WA_MSG_IMAGE:    return "WhatsApp Image Keys";
        case WA_MSG_VIDEO:    return "WhatsApp Video Keys";
        case WA_MSG_AUDIO:    return "WhatsApp Audio Keys";
        case WA_MSG_DOCUMENT: return "WhatsApp Document Keys";
        default:              return NULL;
    }
}

// Fetch the raw encrypted blob for a directPath from the media CDN (token = base64url(fileEncSha256)). 0 or -1.
static int fetch_media_blob(const char *direct_path, const uint8_t *enc_sha, size_t enc_sha_len,
                            const char *ca_file, uint8_t **blob, size_t *blob_len)
{
    char tok[64] = { 0 };
    if (enc_sha && enc_sha_len) {
        size_t n = wa_base64_encode(enc_sha, enc_sha_len, tok, sizeof tok);
        for (size_t i = 0; i < n; i++) { if (tok[i] == '+') tok[i] = '-'; else if (tok[i] == '/') tok[i] = '_'; }
        while (n > 0 && tok[n - 1] == '=') tok[--n] = 0;
    }
    char *path = tok[0]
        ? wa_sprintf("%s%ctoken=%s", direct_path, strchr(direct_path, '?') ? '&' : '?', tok)
        : wa_strdup(direct_path);
    if (!path) return -1;
    int status = 0;
    *blob = wa_https_get_bin(HISTORY_MEDIA_HOST, path, ca_file, 30000, &status, blob_len);
    free(path);
    if (!*blob || status != 200) { wa_log("media: download failed (HTTP %d)", status); free(*blob); *blob = NULL; return -1; }
    return 0;
}

// Download the external history blob from the media CDN and media-decrypt it into `compressed`. 0 or -1.
static int download_history_blob(const char *direct_path, const uint8_t *enc_sha, size_t enc_sha_len,
                                 const uint8_t media_key[32], const char *ca_file, wa_buf *compressed)
{
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    if (fetch_media_blob(direct_path, enc_sha, enc_sha_len, ca_file, &blob, &blob_len) != 0) return -1;
    wa_log("history: downloaded blob %zu bytes, decrypting", blob_len);
    int rc = wa_history_media_decrypt(blob, blob_len, media_key, compressed);
    free(blob);
    return rc;
}

int wa_media_download(const char *direct_path, const uint8_t *media_key, const uint8_t *enc_sha, size_t enc_sha_len,
                      int kind, const char *ca_file, wa_buf *out)
{
    const char *info = wa_media_app_info(kind);
    if (!info || !direct_path || !media_key) return -1;
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    if (fetch_media_blob(direct_path, enc_sha, enc_sha_len, ca_file, &blob, &blob_len) != 0) return -1;
    wa_log("media: downloaded %zu bytes (%s), decrypting", blob_len, info);
    int rc = media_decrypt_info(blob, blob_len, media_key, info, out);
    free(blob);
    return rc;
}

int wa_history_from_message(const uint8_t *message, size_t mlen, const char *ca_file, wa_history_result *out)
{
    memset(out, 0, sizeof *out);   // every return path leaves out valid (zeroed or filled) so the caller can free it
    const uint8_t *pm;
    size_t pml;
    if (!pb_get_bytes(message, mlen, F_MSG_PROTOCOLMESSAGE, &pm, &pml)) return 0;
    const uint8_t *hsn;
    size_t hsnl;
    if (!pb_get_bytes(pm, pml, F_PROTO_HISTORYSYNCNOTIF, &hsn, &hsnl)) return 0;

    // From here a history-sync notification is present: the caller must send the receipt whatever happens.
    uint64_t sync_type = 0;
    int have_sync_type = pb_get_varint(hsn, hsnl, F_HSN_SYNCTYPE, &sync_type);

    // Inline (INITIAL_BOOTSTRAP): the compressed HistorySync travels in the notification, no download.
    const uint8_t *inlinep;
    size_t inlinel;
    if (pb_get_bytes(hsn, hsnl, F_HSN_INLINEPAYLOAD, &inlinep, &inlinel) && inlinel > 0) {
        if (wa_history_parse_compressed(inlinep, inlinel, out) != 0) { wa_log("history: inline parse failed"); return -1; }
        if (out->sync_type == 0 && have_sync_type) out->sync_type = (int)sync_type;
        wa_log("history: inline bootstrap, syncType=%d -> %d conversations, %d mappings", out->sync_type, out->nconvs, out->nmappings);
        return 1;
    }

    // External: download the blob from the CDN and media-decrypt it, then inflate + parse.
    char *direct = pb_get_string(hsn, hsnl, F_HSN_DIRECTPATH);
    const uint8_t *mkey, *enc_sha = NULL;
    size_t mkeyl = 0, enc_shal = 0;
    int have_key = pb_get_bytes(hsn, hsnl, F_HSN_MEDIAKEY, &mkey, &mkeyl) && mkeyl == 32;
    pb_get_bytes(hsn, hsnl, F_HSN_FILEENCSHA256, &enc_sha, &enc_shal);
    if (!direct || !direct[0] || !have_key) {
        free(direct);
        wa_log("history: external blob missing directPath/mediaKey");
        return -1;
    }

    wa_buf compressed;
    wa_buf_init(&compressed);
    int rc = download_history_blob(direct, enc_sha, enc_shal, mkey, ca_file, &compressed);
    free(direct);
    if (rc != 0) { wa_buf_free(&compressed); return -1; }

    rc = wa_history_parse_compressed(compressed.data, compressed.len, out);
    wa_buf_free(&compressed);
    if (rc != 0) return -1;
    if (out->sync_type == 0 && have_sync_type) out->sync_type = (int)sync_type;
    wa_log("history: external blob, syncType=%d -> %d conversations, %d mappings", out->sync_type, out->nconvs, out->nmappings);
    return 1;
}

// -------------------------------------------------------------------------------------------------- AppStateSync (syncd)

int wa_syncd_parse_key_share(const uint8_t *message, size_t mlen, wa_sync_key *out_keys, int max_keys)
{
    if (!out_keys || max_keys <= 0) return 0;
    const uint8_t *pm = NULL, *ks = NULL;
    size_t pml = 0, ksl = 0;
    if (!pb_get_bytes(message, mlen, F_MSG_PROTOCOLMESSAGE, &pm, &pml)) return 0;
    if (!pb_get_bytes(pm, pml, 7, &ks, &ksl)) return 0; // ProtocolMessage.appStateSyncKeyShare = 7
    pb_reader r;
    pb_reader_init(&r, ks, ksl);
    pb_field f;
    int count = 0;
    while (pb_next(&r, &f) > 0 && count < max_keys) {
        if (f.field != 1 || f.wire != PB_BYTES) continue; // AppStateSyncKeyShare.keys = 1
        const uint8_t *kid_msg = NULL, *kdata_msg = NULL, *kid = NULL, *kdata = NULL;
        size_t kid_msgl = 0, kdata_msgl = 0, kidl = 0, kdatal = 0;
        if (!pb_get_bytes(f.data, f.len, 1, &kid_msg, &kid_msgl) ||
            !pb_get_bytes(f.data, f.len, 2, &kdata_msg, &kdata_msgl)) continue;
        if (!pb_get_bytes(kid_msg, kid_msgl, 1, &kid, &kidl) || kidl == 0 || kidl > sizeof(out_keys[0].key_id)) continue;
        if (!pb_get_bytes(kdata_msg, kdata_msgl, 1, &kdata, &kdatal) || kdatal != 32) continue;
        memset(&out_keys[count], 0, sizeof(out_keys[count]));
        memcpy(out_keys[count].key_id, kid, kidl);
        out_keys[count].key_id_len = kidl;
        memcpy(out_keys[count].key_data, kdata, 32);
        count++;
    }
    return count;
}

static void parse_sync_record(const uint8_t *rec, size_t recl, const wa_sync_key *keys, int nkeys,
                              wa_sync_contact **arr, int *cnt, int *cap)
{
    const uint8_t *val_msg = NULL, *kid_msg = NULL, *blob = NULL, *kid = NULL;
    size_t val_msgl = 0, kid_msgl = 0, blobl = 0, kidl = 0;
    if (!pb_get_bytes(rec, recl, 2, &val_msg, &val_msgl) ||
        !pb_get_bytes(rec, recl, 3, &kid_msg, &kid_msgl)) return;
    if (!pb_get_bytes(val_msg, val_msgl, 1, &blob, &blobl) || blobl <= 16 + 32) return;
    if (!pb_get_bytes(kid_msg, kid_msgl, 1, &kid, &kidl) || kidl == 0) return;

    const uint8_t *key_data = NULL;
    for (int i = 0; i < nkeys; i++) {
        if (keys[i].key_id_len == kidl && memcmp(keys[i].key_id, kid, kidl) == 0) {
            key_data = keys[i].key_data;
            break;
        }
    }
    if (!key_data) return;

    uint8_t expanded[160];
    if (wa_hkdf_sha256(NULL, 0, key_data, 32, "WhatsApp Mutation Keys", 22, expanded, sizeof expanded) != 0) return;
    const uint8_t *val_enc_key = expanded + 32;
    const uint8_t *iv = blob;
    const uint8_t *ct = blob + 16;
    size_t ct_len = blobl - 16 - 32;

    wa_buf plain;
    wa_buf_init(&plain);
    if (wa_aes_cbc_decrypt(val_enc_key, iv, ct, ct_len, &plain) == 0) {
        // SyncActionData: index=1 (JSON string), value=2 (SyncActionValue)
        char *idx_json = pb_get_string(plain.data, plain.len, 1);
        const uint8_t *sav = NULL, *ca = NULL;
        size_t savl = 0, cal = 0;
        if (idx_json && pb_get_bytes(plain.data, plain.len, 2, &sav, &savl) &&
            pb_get_bytes(sav, savl, 2, &ca, &cal)) { // SyncActionValue.contactAction = 2
            // Extract JID from index JSON: ["contact","<jid>"]
            char *jid = NULL;
            char *q1 = strchr(idx_json, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) {
                    char *q3 = strchr(q2 + 1, '"');
                    if (q3) {
                        char *q4 = strchr(q3 + 1, '"');
                        if (q4 && q4 > q3 + 1) {
                            size_t jl = (size_t)(q4 - (q3 + 1));
                            jid = (char *)malloc(jl + 1);
                            if (jid) { memcpy(jid, q3 + 1, jl); jid[jl] = 0; }
                        }
                    }
                }
            }
            if (jid) {
                if (*cnt >= *cap) {
                    int ncap = *cap ? (*cap * 2) : 64;
                    wa_sync_contact *np = (wa_sync_contact *)realloc(*arr, (size_t)ncap * sizeof(*np));
                    if (np) { *arr = np; *cap = ncap; }
                }
                if (*cnt < *cap) {
                    wa_sync_contact *c = &(*arr)[(*cnt)++];
                    memset(c, 0, sizeof *c);
                    c->jid = jid;
                    c->full_name = pb_get_string(ca, cal, 1);
                    c->first_name = pb_get_string(ca, cal, 2);
                    c->lid_jid = pb_get_string(ca, cal, 3);
                    c->pn_jid = pb_get_string(ca, cal, 5);
                } else {
                    free(jid);
                }
            }
        }
        free(idx_json);
    }
    wa_buf_free(&plain);
}

int wa_syncd_decrypt_records(const uint8_t *proto, size_t proto_len, int is_patch,
                             const wa_sync_key *keys, int nkeys,
                             wa_sync_contact **out_contacts, int *out_count)
{
    if (!out_contacts || !out_count) return -1;
    *out_contacts = NULL;
    *out_count = 0;
    if (!proto || !proto_len || !keys || nkeys <= 0) return 0;

    int cap = 0;
    pb_reader r;
    pb_reader_init(&r, proto, proto_len);
    pb_field f;
    while (pb_next(&r, &f) > 0) {
        if (f.field != 2 || f.wire != PB_BYTES) continue;
        if (is_patch) {
            // SyncdPatch.mutations (2) -> SyncdMutation.record (2)
            const uint8_t *rec = NULL;
            size_t recl = 0;
            if (pb_get_bytes(f.data, f.len, 2, &rec, &recl))
                parse_sync_record(rec, recl, keys, nkeys, out_contacts, out_count, &cap);
        } else {
            // SyncdSnapshot.records (2) -> SyncdRecord
            parse_sync_record(f.data, f.len, keys, nkeys, out_contacts, out_count, &cap);
        }
    }
    return 0;
}

int wa_syncd_download_and_decrypt_snapshot(const uint8_t *ext_blob_ref, size_t ref_len, const char *ca_file,
                                           const wa_sync_key *keys, int nkeys,
                                           wa_sync_contact **out_contacts, int *out_count)
{
    if (!out_contacts || !out_count) return -1;
    *out_contacts = NULL;
    *out_count = 0;
    const uint8_t *mkey = NULL, *enc_sha = NULL;
    size_t mkeyl = 0, enc_shal = 0;
    char *direct = pb_get_string(ext_blob_ref, ref_len, 2); // ExternalBlobReference.directPath = 2
    int have_key = pb_get_bytes(ext_blob_ref, ref_len, 1, &mkey, &mkeyl) && mkeyl == 32; // mediaKey = 1
    pb_get_bytes(ext_blob_ref, ref_len, 6, &enc_sha, &enc_shal); // fileEncSha256 = 6
    if (!direct || !direct[0] || !have_key) { free(direct); return -1; }

    uint8_t *blob = NULL;
    size_t blob_len = 0;
    int rc = fetch_media_blob(direct, enc_sha, enc_shal, ca_file, &blob, &blob_len);
    free(direct);
    if (rc != 0) return -1;

    wa_buf plain;
    wa_buf_init(&plain);
    rc = media_decrypt_info(blob, blob_len, mkey, "WhatsApp Patch Integrity", &plain);
    free(blob);
    if (rc != 0) { wa_buf_free(&plain); return -1; }

    rc = wa_syncd_decrypt_records(plain.data, plain.len, 0, keys, nkeys, out_contacts, out_count);
    wa_buf_free(&plain);
    return rc;
}

void wa_syncd_contacts_free(wa_sync_contact *contacts, int count)
{
    if (!contacts) return;
    for (int i = 0; i < count; i++) {
        free(contacts[i].jid);
        free(contacts[i].full_name);
        free(contacts[i].first_name);
        free(contacts[i].lid_jid);
        free(contacts[i].pn_jid);
    }
    free(contacts);
}

// -------------------------------------------------------------------------------------------------- self test

// Build a WebMessageInfo { key{remoteJid,id,fromMe=0}, message{...text...}, messageTimestamp }. text_field selects
// the Message field (1 = conversation, 6 = extendedTextMessage.text). Appends the WebMessageInfo bytes to out.
static int st_build_wmi(const char *remote, const char *msgid, int text_field, const char *text, uint64_t ts, wa_buf *out)
{
    wa_buf key, inner, msg;
    wa_buf_init(&key);
    wa_buf_init(&inner);
    wa_buf_init(&msg);
    int ok = pb_put_string(&key, 1, remote) == 0 && pb_put_bool(&key, 2, 0) == 0 && pb_put_string(&key, 3, msgid) == 0;
    if (ok) {
        if (text_field == 1) ok = pb_put_string(&inner, 1, text) == 0;
        else { wa_buf ext; wa_buf_init(&ext); ok = pb_put_string(&ext, 1, text) == 0 && pb_put_message(&inner, 6, &ext) == 0; wa_buf_free(&ext); }
    }
    if (ok) ok = pb_put_message(&msg, 1, &key) == 0 && pb_put_message(&msg, 2, &inner) == 0 && pb_put_varint(&msg, 3, ts) == 0;
    // wrap the WebMessageInfo in a HistorySyncMsg{message=1} and append it as its own Conversation.messages(2) entry
    if (ok) {
        wa_buf hsm;
        wa_buf_init(&hsm);
        ok = pb_put_message(&hsm, F_HSM_MESSAGE, &msg) == 0 && pb_put_message(out, F_CONV_MESSAGES, &hsm) == 0;
        wa_buf_free(&hsm);
    }
    wa_buf_free(&key);
    wa_buf_free(&inner);
    wa_buf_free(&msg);
    return ok ? 0 : -1;
}

// Build a WebMessageInfo carrying an imageMessage { caption, jpegThumbnail }. Appends the HistorySyncMsg bytes to out.
static int st_build_image_wmi(const char *remote, const char *msgid, const char *caption,
                              const uint8_t *thumb, size_t thumb_len, uint64_t ts, wa_buf *out)
{
    wa_buf key, img, inner, msg;
    wa_buf_init(&key);
    wa_buf_init(&img);
    wa_buf_init(&inner);
    wa_buf_init(&msg);
    int ok = pb_put_string(&key, 1, remote) == 0 && pb_put_bool(&key, 2, 0) == 0 && pb_put_string(&key, 3, msgid) == 0;
    if (ok) ok = pb_put_string(&img, F_IMG_CAPTION, caption) == 0 && pb_put_bytes(&img, F_JPEG_THUMBNAIL, thumb, thumb_len) == 0;
    if (ok) ok = pb_put_message(&inner, F_MSG_IMAGE, &img) == 0;   // Message.imageMessage = 3
    if (ok) ok = pb_put_message(&msg, 1, &key) == 0 && pb_put_message(&msg, 2, &inner) == 0 && pb_put_varint(&msg, 3, ts) == 0;
    if (ok) {
        wa_buf hsm;
        wa_buf_init(&hsm);
        ok = pb_put_message(&hsm, F_HSM_MESSAGE, &msg) == 0 && pb_put_message(out, F_CONV_MESSAGES, &hsm) == 0;
        wa_buf_free(&hsm);
    }
    wa_buf_free(&key);
    wa_buf_free(&img);
    wa_buf_free(&inner);
    wa_buf_free(&msg);
    return ok ? 0 : -1;
}

// Build a Conversation { id, (name|displayName), conversationTimestamp, messages... }. `msgsbuf` already holds the
// framed Conversation.messages(2) entries (from st_build_wmi/st_build_image_wmi), so they are appended raw.
static int st_build_conv(const char *id, int name_field, const char *name, uint64_t ts, const wa_buf *msgsbuf, wa_buf *out)
{
    wa_buf conv;
    wa_buf_init(&conv);
    int ok = pb_put_string(&conv, F_CONV_ID, id) == 0 &&
             pb_put_string(&conv, name_field, name) == 0 &&
             pb_put_varint(&conv, F_CONV_TIMESTAMP, ts) == 0 &&
             wa_buf_append(&conv, msgsbuf->data, msgsbuf->len) == 0;
    if (ok) ok = pb_put_message(out, F_HS_CONVERSATIONS, &conv) == 0;
    wa_buf_free(&conv);
    return ok ? 0 : -1;
}

// Build a two-conversation HistorySync protobuf (syncType=0). Appends to out.
static int st_build_history_sync(wa_buf *out)
{
    int ok = pb_put_varint(out, F_HS_SYNCTYPE, 0) == 0;   // INITIAL_BOOTSTRAP

    wa_buf hsm1, hsm2;
    wa_buf_init(&hsm1);
    wa_buf_init(&hsm2);
    static const uint8_t st_thumb[6] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x12, 0x34 };   // fake JPEG header bytes
    if (ok) ok = st_build_wmi("4915551234@s.whatsapp.net", "ID1", 1, "Ahoj, jak se mas?", 1000, &hsm1) == 0;
    if (ok) ok = st_build_image_wmi("4915551234@s.whatsapp.net", "ID3", "Hezka fotka", st_thumb, sizeof st_thumb, 1200, &hsm1) == 0;
    if (ok) ok = st_build_wmi("120363000000000000@g.us", "ID2", 6, "Hi group", 2000, &hsm2) == 0;
    if (ok) ok = st_build_conv("4915551234@s.whatsapp.net", F_CONV_NAME, "Alice", 1500, &hsm1, out) == 0;
    if (ok) ok = st_build_conv("120363000000000000@g.us", F_CONV_DISPLAYNAME, "Parta", 2500, &hsm2, out) == 0;
    wa_buf_free(&hsm1);
    wa_buf_free(&hsm2);
    return ok ? 0 : -1;
}

static int st_zlib_compress(const uint8_t *in, size_t len, wa_buf *out)
{
    uLongf clen = compressBound((uLong)len);
    uint8_t *cbuf = (uint8_t *)malloc(clen ? clen : 1);
    if (!cbuf) return -1;
    int rc = compress2(cbuf, &clen, in, (uLong)len, 6);
    if (rc == Z_OK) rc = wa_buf_append(out, cbuf, clen);
    free(cbuf);
    return rc == 0 || rc == Z_OK ? 0 : -1;
}

static int st_convs_ok(const wa_history_result *r)
{
    if (r->nconvs != 2) return 0;
    const wa_history_conv *a = NULL, *b = NULL;
    for (int i = 0; i < 2; i++) {
        if (strcmp(r->convs[i].jid, "4915551234@s.whatsapp.net") == 0) a = &r->convs[i];
        else if (strcmp(r->convs[i].jid, "120363000000000000@g.us") == 0) b = &r->convs[i];
    }
    if (!a || !b) return 0;
    if (!a->name || strcmp(a->name, "Alice") != 0) return 0;
    if (!a->last || strcmp(a->last, "Hezka fotka") != 0) return 0;   // newest message is the image + caption
    if (!b->name || strcmp(b->name, "Parta") != 0) return 0;
    if (!b->last || strcmp(b->last, "Hi group") != 0) return 0;
    // Alice: a text then an image (oldest-first); check the per-message content + the image thumbnail.
    if (a->nmsgs != 2 || !a->msgs) return 0;
    if (a->msgs[0].kind != WA_MSG_TEXT || !a->msgs[0].text || strcmp(a->msgs[0].text, "Ahoj, jak se mas?") != 0 ||
        a->msgs[0].from_me != 0 || !a->msgs[0].id || strcmp(a->msgs[0].id, "ID1") != 0) return 0;
    if (a->msgs[1].kind != WA_MSG_IMAGE || !a->msgs[1].text || strcmp(a->msgs[1].text, "Hezka fotka") != 0 ||
        a->msgs[1].thumb_len != 6 || !a->msgs[1].thumb || a->msgs[1].thumb[0] != 0xFF || a->msgs[1].thumb[1] != 0xD8) return 0;
    if (b->nmsgs != 1 || !b->msgs || b->msgs[0].kind != WA_MSG_TEXT || !b->msgs[0].text ||
        strcmp(b->msgs[0].text, "Hi group") != 0) return 0;
    return 1;
}

int wa_history_selftest(void)
{
    int ok = 0;
    wa_buf raw, comp;
    wa_buf_init(&raw);
    wa_buf_init(&comp);
    if (st_build_history_sync(&raw) != 0) { wa_log("history selftest: build failed"); goto done; }

    // 1) parse the raw (decompressed) HistorySync.
    wa_history_result r1;
    if (wa_history_parse(raw.data, raw.len, &r1) != 0 || !st_convs_ok(&r1)) {
        wa_log("history selftest: raw parse failed");
        wa_history_result_free(&r1);
        goto done;
    }
    wa_history_result_free(&r1);

    // 2) compress, then parse the compressed blob (the inline-bootstrap path).
    if (st_zlib_compress(raw.data, raw.len, &comp) != 0) { wa_log("history selftest: compress failed"); goto done; }
    wa_history_result r2;
    if (wa_history_parse_compressed(comp.data, comp.len, &r2) != 0 || !st_convs_ok(&r2)) {
        wa_log("history selftest: compressed parse failed");
        wa_history_result_free(&r2);
        goto done;
    }
    wa_history_result_free(&r2);

    // 3) media round trip: encrypt the compressed blob with the History HKDF context, then decrypt + parse.
    {
        uint8_t media_key[32];
        for (int i = 0; i < 32; i++) media_key[i] = (uint8_t)(i * 7 + 3);
        uint8_t exp[112];
        if (wa_hkdf_sha256(NULL, 0, media_key, 32, HISTORY_HKDF_INFO, strlen(HISTORY_HKDF_INFO), exp, sizeof exp) != 0) {
            wa_log("history selftest: hkdf failed");
            goto done;
        }
        wa_buf ct, blob, dec;
        wa_buf_init(&ct);
        wa_buf_init(&blob);
        wa_buf_init(&dec);
        int mok = wa_aes_cbc_encrypt(exp + 16, exp, comp.data, comp.len, &ct) == 0;
        if (mok) {
            const void *parts[2] = { exp, ct.data };
            size_t lens[2] = { 16, ct.len };
            uint8_t mac[32];
            wa_hmac_sha256_v(exp + 48, 32, 2, parts, lens, mac);
            mok = wa_buf_append(&blob, ct.data, ct.len) == 0 && wa_buf_append(&blob, mac, MEDIA_MAC_SIZE) == 0;
        }
        if (mok) mok = wa_history_media_decrypt(blob.data, blob.len, media_key, &dec) == 0 &&
                       dec.len == comp.len && memcmp(dec.data, comp.data, comp.len) == 0;
        wa_history_result r3;
        if (mok && wa_history_parse_compressed(dec.data, dec.len, &r3) == 0 && st_convs_ok(&r3)) {
            wa_history_result_free(&r3);
        } else {
            if (mok) wa_history_result_free(&r3);
            wa_buf_free(&ct);
            wa_buf_free(&blob);
            wa_buf_free(&dec);
            wa_log("history selftest: media round trip failed");
            goto done;
        }
        wa_buf_free(&ct);
        wa_buf_free(&blob);
        wa_buf_free(&dec);
    }

    // 4) wrap the compressed blob as an inline historySyncNotification inside a Message, and run the message path.
    {
        wa_buf hsn, proto, msg;
        wa_buf_init(&hsn);
        wa_buf_init(&proto);
        wa_buf_init(&msg);
        int wok = pb_put_varint(&hsn, F_HSN_SYNCTYPE, 0) == 0 &&
                  pb_put_bytes(&hsn, F_HSN_INLINEPAYLOAD, comp.data, comp.len) == 0 &&
                  pb_put_message(&proto, F_PROTO_HISTORYSYNCNOTIF, &hsn) == 0 &&
                  pb_put_message(&msg, F_MSG_PROTOCOLMESSAGE, &proto) == 0;
        wa_history_result r4;
        memset(&r4, 0, sizeof r4);
        int mret = wok ? wa_history_from_message(msg.data, msg.len, NULL, &r4) : -1;
        ok = (mret == 1) && st_convs_ok(&r4);
        wa_history_result_free(&r4);
        wa_buf_free(&hsn);
        wa_buf_free(&proto);
        wa_buf_free(&msg);
        if (!ok) { wa_log("history selftest: message path failed"); goto done; }
    }

done:
    wa_buf_free(&raw);
    wa_buf_free(&comp);
    return ok ? 0 : -1;
}
