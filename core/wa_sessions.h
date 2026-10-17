// A store of Signal sessions keyed by device JID, persisted to a file. wa_sig_session is a fixed-size POD, so the
// whole set serializes as a flat blob.
#ifndef WA_SESSIONS_H
#define WA_SESSIONS_H

#include "wa_signal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WA_SESS_MAX 512

typedef struct {
    char jid[80];
    wa_sig_session sess;
    int used;
} wa_sess_entry;

typedef struct {
    wa_sess_entry entries[WA_SESS_MAX];
    int count;
} wa_sess_store;

void wa_sess_init(wa_sess_store *s);
// Returns the session for a device JID, or NULL. The pointer is stable until the next put.
wa_sig_session *wa_sess_get(wa_sess_store *s, const char *jid);
// Inserts or replaces the session for a device JID. Returns 0 or -1 (full).
int wa_sess_put(wa_sess_store *s, const char *jid, const wa_sig_session *sess);
int wa_sess_has(const wa_sess_store *s, const char *jid);
void wa_sess_del(wa_sess_store *s, const char *jid);
void wa_sess_del_user(wa_sess_store *s, const char *user_prefix);

int wa_sess_save(const wa_sess_store *s, const char *path);
int wa_sess_load(wa_sess_store *s, const char *path);   // -1 when missing/corrupt

int wa_sessions_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
