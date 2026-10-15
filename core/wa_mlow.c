#include "wa_mlow.h"
#include "wa_util.h"

#include <string.h>

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

// -------------------------------------------------------------------------------------------------- LSF decode

extern const uint16_t wa_lsf_cdf[];
extern const int wa_lsf_off[];
extern const int wa_lsf_len[];
extern const int wa_lsf_stage2_base;
extern const int wa_lsf_cfg_count;
extern const int wa_lsf_grid_count;

static int lsf_cdf(wa_rc *dec, int idx)
{
    return wa_rc_decode_cdf(dec, &wa_lsf_cdf[wa_lsf_off[idx]], (size_t)wa_lsf_len[idx]);
}

void wa_mlow_decode_lsf(wa_rc *dec, int config, int intf, int active, wa_mlow_lsf_state *st, wa_mlow_lsf_indices *out)
{
    int sel = intf == 0 ? 0 : (st->prev_stage1 != 0 ? 2 : 1);
    int stage1 = active ? lsf_cdf(dec, sel) : 0;
    out->stage1 = stage1;

    int enter_match = intf != 0;
    int m = enter_match && (stage1 == st->prev_stage1);
    if (!m) { st->prev_gain_idx = -1; st->prev_filt_idx = -1; st->prev_lag = -1; st->prev_frac_lag = -1; }
    st->prev_stage1 = stage1;

    // grid cdf: m?(s1?match1:match1_alt):(s1?match0_alt:match0)  => indices 3/4/6/5
    int grid_idx = m ? (stage1 != 0 ? 3 : 4) : (stage1 != 0 ? 6 : 5);
    out->grid = lsf_cdf(dec, grid_idx);
    st->prev_match = m;
    st->have_prev = 1;

    int base = wa_lsf_stage2_base + (((stage1 * wa_lsf_cfg_count + config) * wa_lsf_grid_count + out->grid) * 16);
    for (int k = 0; k < 16; k++) out->stage2[k] = lsf_cdf(dec, base + k);

    out->extra = active ? lsf_cdf(dec, 7) : 0;
}

extern const int wa_lsf_kat_len;
extern const int wa_lsf_kat_framelen[];
extern const int wa_lsf_kat_frameoff[];
extern const uint8_t wa_lsf_kat_frames[];
extern const int wa_lsf_kat_stage1[];
extern const int wa_lsf_kat_grid[];
extern const int wa_lsf_kat_extra[];
extern const int wa_lsf_kat_stage2[][16];

int wa_mlow_lsf_selftest(void)
{
    for (int i = 0; i < wa_lsf_kat_len; i++) {
        const uint8_t *frame = &wa_lsf_kat_frames[wa_lsf_kat_frameoff[i]];
        int flen = wa_lsf_kat_framelen[i];
        wa_rc dec;
        wa_rc_init(&dec, frame + 1, (size_t)(flen - 1));   // skip the TOC byte (the vector decodes from body start)
        wa_mlow_lsf_state st;
        memset(&st, 0, sizeof st);
        wa_mlow_lsf_indices idx;
        wa_mlow_decode_lsf(&dec, 0, 0, 1, &st, &idx);
        int ok = idx.stage1 == wa_lsf_kat_stage1[i] && idx.grid == wa_lsf_kat_grid[i] && idx.extra == wa_lsf_kat_extra[i] && dec.err == 0;
        for (int k = 0; k < 16 && ok; k++) if (idx.stage2[k] != wa_lsf_kat_stage2[i][k]) ok = 0;
        if (!ok) { wa_log("mlow lsf kat %d: stage1=%d/%d grid=%d/%d extra=%d/%d err=%d", i, idx.stage1, wa_lsf_kat_stage1[i], idx.grid, wa_lsf_kat_grid[i], idx.extra, wa_lsf_kat_extra[i], dec.err); return -1; }
    }
    return 0;
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
