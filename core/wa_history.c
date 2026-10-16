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
#define F_CONV_ID                   1
#define F_CONV_MESSAGES             2
#define F_CONV_LASTMSGTS            5
#define F_CONV_UNREAD               6
#define F_CONV_TIMESTAMP           12
#define F_CONV_NAME                13
#define F_CONV_DISPLAYNAME         38
#define F_HSM_MESSAGE               1
#define F_WMI_MESSAGE               2
#define F_WMI_MESSAGETIMESTAMP      3

#define HISTORY_MEDIA_HOST   "mmg.whatsapp.net"
#define HISTORY_HKDF_INFO    "WhatsApp History Keys"
#define MEDIA_MAC_SIZE       10

static void conv_free(wa_history_conv *c)
{
    free(c->jid);
    free(c->name);
    free(c->last);
}

void wa_history_result_free(wa_history_result *r)
{
    if (!r) return;
    for (int i = 0; i < r->nconvs; i++) conv_free(&r->convs[i]);
    free(r->convs);
    r->convs = NULL;
    r->nconvs = 0;
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

    uint64_t ts = 0;
    if (pb_get_varint(c, cl, F_CONV_TIMESTAMP, &ts)) out->timestamp = ts;
    else if (pb_get_varint(c, cl, F_CONV_LASTMSGTS, &ts)) out->timestamp = ts;
    uint64_t unread = 0;
    if (pb_get_varint(c, cl, F_CONV_UNREAD, &unread)) out->unread = (uint32_t)unread;

    // The last message's text: scan the repeated messages (HistorySyncMsg), keep the newest that carries text.
    pb_reader r;
    pb_reader_init(&r, c, cl);
    pb_field f;
    uint64_t best_ts = 0;
    char *best_text = NULL;
    int rc;
    while ((rc = pb_next(&r, &f)) > 0) {
        if (f.field != F_CONV_MESSAGES || f.wire != PB_BYTES) continue;
        const uint8_t *wmi;
        size_t wmil;
        if (!pb_get_bytes(f.data, f.len, F_HSM_MESSAGE, &wmi, &wmil)) continue;   // HistorySyncMsg.message -> WebMessageInfo
        const uint8_t *inner;
        size_t innerl;
        char *text = NULL;
        if (pb_get_bytes(wmi, wmil, F_WMI_MESSAGE, &inner, &innerl)) text = wa_msg_parse_text(inner, innerl);
        uint64_t mts = 0;
        pb_get_varint(wmi, wmil, F_WMI_MESSAGETIMESTAMP, &mts);
        if (text) {
            if (!best_text || mts >= best_ts) { free(best_text); best_text = text; best_ts = mts; }
            else free(text);
        }
    }
    out->last = best_text;
    if (out->timestamp == 0 && best_ts) out->timestamp = best_ts;
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
    int cap = 0, rc;
    while ((rc = pb_next(&r, &f)) > 0) {
        if (f.field != F_HS_CONVERSATIONS || f.wire != PB_BYTES) continue;
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

int wa_history_media_decrypt(const uint8_t *blob, size_t blob_len, const uint8_t media_key[32], wa_buf *out)
{
    if (blob_len <= MEDIA_MAC_SIZE) return -1;
    uint8_t expanded[112];
    if (wa_hkdf_sha256(NULL, 0, media_key, 32, HISTORY_HKDF_INFO, strlen(HISTORY_HKDF_INFO), expanded, sizeof expanded) != 0)
        return -1;
    const uint8_t *iv = expanded;
    const uint8_t *cipher_key = expanded + 16;
    const uint8_t *mac_key = expanded + 48;
    size_t ct_len = blob_len - MEDIA_MAC_SIZE;

    // HMAC-SHA256(mac_key, iv || ciphertext); the first 10 bytes must equal the blob's trailing 10.
    const void *parts[2] = { iv, blob };
    size_t lens[2] = { 16, ct_len };
    uint8_t mac[32];
    wa_hmac_sha256_v(mac_key, 32, 2, parts, lens, mac);
    if (wa_ct_compare(mac, blob + ct_len, MEDIA_MAC_SIZE) != 0) { wa_log("history: media MAC mismatch"); return -1; }

    return wa_aes_cbc_decrypt(cipher_key, iv, blob, ct_len, out);
}

// Download the external history blob from the media CDN and media-decrypt it into `compressed`. 0 or -1.
static int download_history_blob(const char *direct_path, const uint8_t *enc_sha, size_t enc_sha_len,
                                 const uint8_t media_key[32], const char *ca_file, wa_buf *compressed)
{
    // token = base64url-no-pad(fileEncSha256); appended to directPath's query.
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
    size_t blob_len = 0;
    uint8_t *blob = wa_https_get_bin(HISTORY_MEDIA_HOST, path, ca_file, 30000, &status, &blob_len);
    free(path);
    if (!blob || status != 200) {
        wa_log("history: blob download failed (HTTP %d, %zu bytes)", status, blob_len);
        free(blob);
        return -1;
    }
    wa_log("history: downloaded blob %zu bytes, decrypting", blob_len);
    int rc = wa_history_media_decrypt(blob, blob_len, media_key, compressed);
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
        wa_log("history: inline bootstrap, syncType=%d -> %d conversations", out->sync_type, out->nconvs);
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
    wa_log("history: external blob, syncType=%d -> %d conversations", out->sync_type, out->nconvs);
    return 1;
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
    if (ok) ok = pb_put_message(out, F_HSM_MESSAGE, &msg) == 0;   // HistorySyncMsg.message
    wa_buf_free(&key);
    wa_buf_free(&inner);
    wa_buf_free(&msg);
    return ok ? 0 : -1;
}

// Build a Conversation { id, (name|displayName), conversationTimestamp, messages[hsm] }. Appends to out as field 2.
static int st_build_conv(const char *id, int name_field, const char *name, uint64_t ts, const wa_buf *hsm, wa_buf *out)
{
    wa_buf conv;
    wa_buf_init(&conv);
    int ok = pb_put_string(&conv, F_CONV_ID, id) == 0 &&
             pb_put_string(&conv, name_field, name) == 0 &&
             pb_put_varint(&conv, F_CONV_TIMESTAMP, ts) == 0 &&
             pb_put_message(&conv, F_CONV_MESSAGES, hsm) == 0;
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
    if (ok) ok = st_build_wmi("4915551234@s.whatsapp.net", "ID1", 1, "Ahoj, jak se mas?", 1000, &hsm1) == 0;
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
    if (!a->last || strcmp(a->last, "Ahoj, jak se mas?") != 0) return 0;
    if (!b->name || strcmp(b->name, "Parta") != 0) return 0;
    if (!b->last || strcmp(b->last, "Hi group") != 0) return 0;
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
