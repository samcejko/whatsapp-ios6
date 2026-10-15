// Messaging orchestration: resolving a contact's devices, establishing Signal sessions from fetched prekeys,
// fanning an encrypted text out to them, and decrypting an incoming message. These run ON THE CLIENT THREAD
// (they use wa_client_request_sync), i.e. from inside a wa_client_post action.
#ifndef WA_CHAT_H
#define WA_CHAT_H

#include "wa_client.h"
#include "wa_sessions.h"

#ifdef __cplusplus
extern "C" {
#endif

// Send a 1:1 text to a bare user JID ("<num>@s.whatsapp.net"). Resolves devices, ensures sessions (prekey fetch),
// encrypts per device, sends the fanned-out <message>. Returns 0 (reached >=1 recipient device) or -1.
int wa_chat_send_text(wa_client *c, wa_sess_store *sessions, const char *to_user_jid, const char *text);

// Decrypt an incoming <message>. On success returns the malloc'd text (caller frees) and writes the sender's bare
// JID into frombuf (>=80). Returns NULL when there is nothing to show (no text / decrypt failed).
char *wa_chat_handle_message(wa_client *c, wa_sess_store *sessions, const wa_node *message, char *frombuf, size_t fromcap);

// Encrypt a 32-byte call key to one peer device's session (for a call <offer> <enc>). Ensures a session first.
// Returns 0 with the ciphertext appended to out and *is_prekey set, or -1.
int wa_chat_encrypt_callkey(wa_client *c, wa_sess_store *sessions, const char *device_jid,
                            const uint8_t call_key[32], int *is_prekey, wa_buf *out);

#ifdef __cplusplus
}
#endif
#endif
