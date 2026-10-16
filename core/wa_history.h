// History sync: after linking, the phone sends a protocolMessage/historySyncNotification (category="peer") whose
// payload is a zlib-compressed HistorySync protobuf — either inline (INITIAL_BOOTSTRAP) or an encrypted blob on the
// media CDN. This module detects it in a decrypted Message, obtains the HistorySync bytes (inline or download +
// media-decrypt), and parses them into the conversations the UI shows as the initial chat list. Plain C99.
#ifndef WA_HISTORY_H
#define WA_HISTORY_H

#include <stdint.h>
#include <stddef.h>
#include "wa_util.h"

#ifdef __cplusplus
extern "C" {
#endif

// Message kinds surfaced from history (what the bubble should show).
enum {
    WA_MSG_TEXT = 0,
    WA_MSG_IMAGE,
    WA_MSG_VIDEO,
    WA_MSG_AUDIO,       // voice note or audio clip
    WA_MSG_DOCUMENT,
    WA_MSG_STICKER,
    WA_MSG_LOCATION,
    WA_MSG_CONTACT,
};

// One message from a conversation's history. Strings/bytes are malloc'd (any may be NULL).
typedef struct {
    char *text;          // text (WA_MSG_TEXT) or the media caption (may be NULL)
    char *id;            // message id (MessageKey.id), or NULL
    uint64_t timestamp;  // messageTimestamp (seconds)
    int from_me;         // 1 when we sent it (MessageKey.fromMe)
    int kind;            // WA_MSG_*
    uint8_t *thumb;      // inline JPEG thumbnail (image/video/document), or NULL
    size_t thumb_len;
    uint32_t duration;   // seconds (audio/video), else 0
    char *filename;      // document file name, or NULL
    char *sender;        // sender display name/number (group messages; pushName or participant), or NULL
    // media download reference (image/video/audio/document) so the full file can be fetched + decrypted on tap:
    char *direct_path;   // CDN path, or NULL
    uint8_t *media_key;  // 32 bytes, or NULL
    size_t media_key_len;
    uint8_t *enc_sha;    // fileEncSha256 (URL token), or NULL
    size_t enc_sha_len;
} wa_history_msg;

// HKDF "app info" strings for WhatsApp media decryption, by kind (for full-media download). NULL for non-media.
const char *wa_media_app_info(int kind);

// Download a WhatsApp media file from the CDN and decrypt it (HKDF(media_key, app_info) -> iv/cipher/mac, verify the
// trailing 10-byte HMAC over iv||ciphertext, AES-256-CBC). Appends the plaintext file to out. 0 or -1.
// direct_path: the message's directPath; enc_sha: fileEncSha256 (URL token, may be NULL). No zlib (unlike history).
int wa_media_download(const char *direct_path, const uint8_t *media_key, const uint8_t *enc_sha, size_t enc_sha_len,
                      int kind, const char *ca_file, wa_buf *out);

// One conversation surfaced from a HistorySync chunk. The strings/arrays are malloc'd (name/last/msgs may be NULL)
// and owned by the wa_history_result; free the whole result with wa_history_result_free.
typedef struct {
    char *jid;           // conversation id (remote JID), e.g. "4915...@s.whatsapp.net" or "...@g.us"
    char *name;          // display name (Conversation.name / .displayName), or NULL
    char *last;          // text of the most recent message, or NULL
    uint64_t timestamp;  // conversationTimestamp / lastMsgTimestamp / newest messageTimestamp (seconds) for sorting
    uint32_t unread;     // unreadCount, or 0
    wa_history_msg *msgs; // text messages, oldest-first (NULL when none)
    int nmsgs;
} wa_history_conv;

typedef struct {
    wa_history_conv *convs;
    int nconvs;
    int sync_type;       // HistorySync.syncType (0 = INITIAL_BOOTSTRAP, 3 = RECENT, 2 = FULL, ...)
} wa_history_result;

void wa_history_result_free(wa_history_result *r);

// Parse a DECOMPRESSED HistorySync protobuf into conversations. 0 on success (out filled; may be empty), -1 on error.
int wa_history_parse(const uint8_t *data, size_t len, wa_history_result *out);

// zlib-inflate a compressed HistorySync blob, then parse it. 0 or -1.
int wa_history_parse_compressed(const uint8_t *in, size_t len, wa_history_result *out);

// Decrypt a WhatsApp "History" media blob in place-ish: HKDF(media_key, "WhatsApp History Keys") -> iv/cipher/mac,
// verify the trailing 10-byte HMAC over iv||ciphertext, AES-256-CBC decrypt (PKCS#7). Appends plaintext to out. 0/-1.
int wa_history_media_decrypt(const uint8_t *blob, size_t blob_len, const uint8_t media_key[32], wa_buf *out);

// Detect protocolMessage(12) -> historySyncNotification(6) in a decrypted Message. When present, obtain the
// HistorySync (inline initialHistBootstrapInlinePayload, else download+decrypt the media blob) and parse it into out.
// Returns 1 when a notification was handled (out valid; the caller should send the hist_sync receipt), 0 when the
// Message carries no history-sync notification (out untouched), -1 when a notification was present but could not be
// obtained/parsed (the caller should still send the receipt so the phone stops re-sending the chunk).
// ca_file: PEM bundle for the CDN TLS, or NULL to skip verification (the blob's own HMAC authenticates the content).
int wa_history_from_message(const uint8_t *message, size_t mlen, const char *ca_file, wa_history_result *out);

int wa_history_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
