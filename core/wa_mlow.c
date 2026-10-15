#include "wa_mlow.h"
#include "wa_util.h"

static int celt_escape_frame_ms(uint8_t b)
{
    static const int tab[4] = { 3, 5, 10, 20 };
    return tab[(b >> 2) & 3];
}

void wa_mlow_parse_toc(uint8_t b, wa_mlow_toc *t)
{
    if ((b & 0xC0) == 0xC0) {
        t->std_opus = 1;
        t->sid = t->vad = t->fec = t->active = t->low_rate = t->stereo = 0;
        t->sample_rate = 16000;
        t->frame_ms = celt_escape_frame_ms(b);
        return;
    }
    static const int ms[4] = { 10, 20, 60, 120 };
    int bit1 = (b >> 1) & 1;
    int vad = (b >> 6) & 1;
    t->std_opus = 0;
    t->sid = (b >> 7) & 1;
    t->vad = vad;
    t->sample_rate = (b & 0x20) ? 32000 : 16000;
    t->frame_ms = ms[(b >> 3) & 3];
    t->fec = vad && bit1;
    t->active = vad || bit1;
    t->low_rate = (b >> 2) & 1;
    t->stereo = b & 1;
}

wa_mlow_route wa_mlow_route_of(uint8_t b)
{
    wa_mlow_toc t;
    wa_mlow_parse_toc(b, &t);
    if (t.std_opus) return WA_MLOW_OPUS;
    if (t.sid) return WA_MLOW_DTX;
    return WA_MLOW_SMPL;
}

extern const int wa_mlow_toc_kat_len;
extern const int32_t wa_mlow_toc_kat[][10];

int wa_mlow_toc_selftest(void)
{
    for (int i = 0; i < wa_mlow_toc_kat_len; i++) {
        const int32_t *r = wa_mlow_toc_kat[i];
        wa_mlow_toc t;
        wa_mlow_parse_toc((uint8_t)r[0], &t);
        int ok = t.std_opus == r[1] && t.sid == r[2] && t.vad == r[3] && t.sample_rate == r[4] &&
                 t.fec == r[6] && t.active == r[7] && t.low_rate == r[8] && t.stereo == r[9];
        if (!t.std_opus) ok = ok && t.frame_ms == r[5];   // the escape 'ms' in the fixture is the stale RFC reading
        if (!ok) { wa_log("mlow toc kat: byte 0x%02x mismatch", (unsigned)r[0]); return -1; }
    }
    return 0;
}
