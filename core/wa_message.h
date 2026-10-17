// 1:1 messaging: the Message protobuf (plain text and the call-key wrapper), Signal encryption of it, and the
// <message>/<enc> stanza it travels in. The per-device fanout (usync + prekey fetch to every recipient device)
// is driven by the client at run time; these are the building blocks and the 1:1 path.
#ifndef WA_MESSAGE_H
#define WA_MESSAGE_H

#include <stdint.h>
#include <stddef.h>
#include "wa_util.h"
#include "wa_node.h"
#include "wa_signal.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build the Message protobuf for a plain text (field conversation=1). Appends to out.
int wa_msg_text_plaintext(const char *text, wa_buf *out);
// Build ExtendedTextMessage (field 6) with optional quoted reply contextInfo (field 17).
int wa_msg_ext_text_plaintext(const char *text, const char *quoted_id, const char *quoted_sender,
                              const char *quoted_text, wa_buf *out);
// Build ReactionMessage (field 46) targeting a specific message key.
int wa_msg_reaction_plaintext(const char *target_id, const char *remote_jid, int from_me,
                              const char *emoji, int64_t ts_ms, wa_buf *out);
// Build the DeviceSentMessage protobuf (field 31): wraps inner_msg with destinationJid. Appends to out.
int wa_msg_dsm_plaintext(const char *destination_jid, const uint8_t *inner_msg, size_t inner_len, wa_buf *out);
// Build the Message protobuf carrying a call key (Message.call.callKey) for a call <offer> <enc>.
int wa_msg_callkey_plaintext(const uint8_t call_key[32], wa_buf *out);

// Signal-encrypt a Message plaintext (adds WhatsApp padding first). Sets *is_prekey. Appends ciphertext to enc_out.
int wa_msg_encrypt(wa_sig_session *s, const wa_sig_keypair *our_identity, const uint8_t *plaintext, size_t len,
                   int *is_prekey, wa_buf *enc_out);
// Signal-decrypt an <enc> body (is_prekey selects the path) and strip the padding. Appends the Message bytes to out.
int wa_msg_decrypt(wa_sig_session *s, const wa_sig_keypair *our_identity, const wa_sig_keypair *our_signed_prekey,
                   const wa_sig_keypair *our_one_time, int is_prekey, const uint8_t *enc, size_t len, wa_buf *out);

// Extract the plain text (conversation / extendedTextMessage / containers) from a decrypted Message. malloc'd, or NULL.
char *wa_msg_parse_text(const uint8_t *message, size_t len);
// Extract destinationJid if message is wrapped in DeviceSentMessage (field 31). malloc'd, or NULL.
char *wa_msg_parse_dsm_dest(const uint8_t *message, size_t len);
// Extract quoted reply stanzaId and quoted text from contextInfo if present. Returns 1 if found.
int wa_msg_parse_quoted(const uint8_t *message, size_t len, char *quoted_id_out, size_t id_cap,
                        char *quoted_text_out, size_t text_cap);
// Extract ReactionMessage (field 46) target message ID and emoji text. Returns 1 if message is a reaction.
int wa_msg_parse_reaction(const uint8_t *message, size_t len, char *target_id_out, size_t id_cap,
                          char *emoji_out, size_t emoji_cap);

// Build a <message to=.. id=.. type=text><enc v=2 type=.. count=0>enc</enc></message> node (caller frees).
wa_node *wa_msg_build_node(const char *to, const char *id, const uint8_t *enc, size_t enc_len, const char *enc_type);

// One recipient/own device's ciphertext in a fanned-out DM.
typedef struct { const char *jid; const uint8_t *enc; size_t enc_len; const char *enc_type; } wa_msg_devenc;
// Build a fanned-out 1:1 message: <message to=user id type=text><participants><to jid><enc/></to>...</participants>
// [<device-identity>]. device_identity (from our pairing) is included when any enc is a pkmsg. Caller frees.
wa_node *wa_msg_build_dm(const char *to_user, const char *id, const wa_msg_devenc *devs, int ndevs,
                         const uint8_t *device_identity, size_t di_len, const char *peer_recipient_pn);

// Parse an incoming <message>: its from/id/type and the first <enc> (type + bytes). Returns 1 on success.
typedef struct {
    const char *from, *id, *type, *notify;
    const char *enc_type;        // "pkmsg" or "msg"
    const uint8_t *enc;
    size_t enc_len;
} wa_msg_incoming;
int wa_msg_parse_incoming(const wa_node *message, wa_msg_incoming *out);

int wa_message_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
