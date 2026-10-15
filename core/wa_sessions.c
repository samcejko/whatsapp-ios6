#include "wa_sessions.h"
#include "wa_util.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SESS_MAGIC 0x57533153u   // "WS1S"

void wa_sess_init(wa_sess_store *s) { memset(s, 0, sizeof *s); }

static int find(const wa_sess_store *s, const char *jid)
{
    for (int i = 0; i < WA_SESS_MAX; i++) if (s->entries[i].used && strcmp(s->entries[i].jid, jid) == 0) return i;
    return -1;
}

wa_sig_session *wa_sess_get(wa_sess_store *s, const char *jid)
{
    int i = find(s, jid);
    return i >= 0 ? &s->entries[i].sess : NULL;
}

int wa_sess_has(const wa_sess_store *s, const char *jid) { return find(s, jid) >= 0; }

int wa_sess_put(wa_sess_store *s, const char *jid, const wa_sig_session *sess)
{
    if (strlen(jid) >= sizeof s->entries[0].jid) return -1;
    int i = find(s, jid);
    if (i < 0) for (i = 0; i < WA_SESS_MAX; i++) if (!s->entries[i].used) break;
    if (i >= WA_SESS_MAX) return -1;
    if (!s->entries[i].used) { s->count++; s->entries[i].used = 1; snprintf(s->entries[i].jid, sizeof s->entries[i].jid, "%s", jid); }
    s->entries[i].sess = *sess;
    return 0;
}

int wa_sess_save(const wa_sess_store *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint8_t hdr[8];
    wa_put_u32(hdr, SESS_MAGIC);
    wa_put_u32(hdr + 4, (uint32_t)sizeof(wa_sig_session));
    int ok = fwrite(hdr, 1, 8, f) == 8;
    for (int i = 0; i < WA_SESS_MAX && ok; i++) {
        if (!s->entries[i].used) continue;
        uint8_t jl[2];
        uint16_t n = (uint16_t)strlen(s->entries[i].jid);
        wa_put_u16(jl, n);
        ok = fwrite(jl, 1, 2, f) == 2 && fwrite(s->entries[i].jid, 1, n, f) == n &&
             fwrite(&s->entries[i].sess, 1, sizeof(wa_sig_session), f) == sizeof(wa_sig_session);
    }
    fclose(f);
    return ok ? 0 : -1;
}

int wa_sess_load(wa_sess_store *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    wa_sess_init(s);
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8 || wa_get_u32(hdr) != SESS_MAGIC || wa_get_u32(hdr + 4) != sizeof(wa_sig_session)) { fclose(f); return -1; }
    for (;;) {
        uint8_t jl[2];
        if (fread(jl, 1, 2, f) != 2) break;
        uint16_t n = wa_get_u16(jl);
        char jid[80];
        wa_sig_session sess;
        if (n >= sizeof jid || fread(jid, 1, n, f) != n || fread(&sess, 1, sizeof sess, f) != sizeof sess) { fclose(f); return -1; }
        jid[n] = 0;
        wa_sess_put(s, jid, &sess);
    }
    fclose(f);
    return 0;
}

int wa_sessions_selftest(void)
{
    wa_sess_store *store = (wa_sess_store *)malloc(sizeof *store);   // ~hundreds of KB: too big for the stack
    if (!store) return -1;
    wa_sess_store *s = store;
    wa_sess_init(s);
    wa_sig_session a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.established = 1;
    a.send_index = 5;
    memset(a.root, 0xAB, 32);
    b.established = 1;
    b.send_index = 9;
    wa_sess_put(s, "1:0@s.whatsapp.net", &a);
    wa_sess_put(s, "1:1@s.whatsapp.net", &b);
    int ok = s->count == 2 && wa_sess_has(s, "1:0@s.whatsapp.net") &&
             wa_sess_get(s, "1:1@s.whatsapp.net")->send_index == 9 && !wa_sess_has(s, "2:0@s.whatsapp.net");
    // replace
    a.send_index = 7;
    wa_sess_put(s, "1:0@s.whatsapp.net", &a);
    ok = ok && s->count == 2 && wa_sess_get(s, "1:0@s.whatsapp.net")->send_index == 7;
    char tmp[L_tmpnam + 8];
    if (ok && tmpnam(tmp)) {
        wa_sess_store *s2 = (wa_sess_store *)malloc(sizeof *s2);
        ok = s2 && wa_sess_save(s, tmp) == 0 && wa_sess_load(s2, tmp) == 0 && s2->count == 2 &&
             wa_sess_get(s2, "1:0@s.whatsapp.net")->send_index == 7 &&
             wa_sess_get(s2, "1:1@s.whatsapp.net")->send_index == 9 &&
             memcmp(wa_sess_get(s2, "1:0@s.whatsapp.net")->root, a.root, 32) == 0;
        remove(tmp);
        free(s2);
    }
    free(store);
    if (!ok) { wa_log("sessions selftest failed"); return -1; }
    return 0;
}
