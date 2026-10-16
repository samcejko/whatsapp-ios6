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

// One text message from a conversation's history. Strings are malloc'd (id may be NULL).
typedef struct {
    char *text;          // message text (conversation / extendedTextMessage)
    char *id;            // message id (MessageKey.id), or NULL
    uint64_t timestamp;  // messageTimestamp (seconds)
    int from_me;         // 1 when we sent it (MessageKey.fromMe)
} wa_history_msg;

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
