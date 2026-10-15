#include "wa_mlow.h"
#include "wa_mlow_cc.h"
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

// -------------------------------------------------------------------------------------------------- pulse decode

// The one static rodata table the pulse path needs (0xe8990..0xe8998), indexed [config*3 + (p4+s1)].
static int mem8_static(uint32_t addr)
{
    static const uint8_t t[8] = { 80, 160, 160, 16, 32, 32, 0, 0 };
    if (addr >= 0xe8990u && addr < 0xe8998u) return t[addr - 0xe8990u];
    return 0;
}

// split one `count`-pulse run across a range; returns the first-half count, or -1 (corrupt).
static int smpl_split3537(wa_rc *dec, int count, int gran)
{
    int lo = count < gran ? count : gran;
    int minSplit = count - gran;
    if (minSplit < 0) minSplit = 0;
    if (lo < minSplit) return -1;
    if (minSplit == lo) return minSplit;
    size_t n;
    const uint16_t *cmf = wa_cc_split_cmf(count, &n);
    if (!cmf) return -1;
    return wa_rc_decode_cdf_window(dec, cmf, n, minSplit, (lo - minSplit) + 2) + minSplit;
}

// recursive binary split of `total` pulses across p3 subframes; fills split[0..p3). Returns 0 or -1 (corrupt).
static int decode_split(wa_rc *dec, int total, int subfrLen16, int p3, int32_t split[8])
{
    memset(split, 0, 8 * sizeof(int32_t));
    if (total == 0) return 0;
    if (p3 == 2) {
        int s0 = smpl_split3537(dec, total, subfrLen16);
        if (s0 < 0) return -1;
        split[0] = s0;
        split[1] = total - s0;
        return 0;
    }
    int sum = total - subfrLen16 * 2;
    if (sum < 0) sum = 0;
    int lo = total - 80;
    if (lo < 0) lo = 0;
    if (sum < lo) return -1;
    int hiBound = total - lo;
    if (sum < hiBound) {
        size_t n;
        const uint16_t *cmf = wa_cc_split_cmf(total, &n);
        if (!cmf) return -1;
        sum += wa_rc_decode_cdf_window(dec, cmf, n, sum - lo, (hiBound - sum) + 2);
    }
    if (sum > 0) { int s0 = smpl_split3537(dec, sum, subfrLen16); split[0] = s0; split[1] = sum - s0; }
    if (sum < total) { int s2 = smpl_split3537(dec, total - sum, subfrLen16); split[2] = s2; split[3] = (total - sum) - s2; }
    if (split[0] == -1 || split[2] == -1) memset(split, 0, 8 * sizeof(int32_t));
    return 0;
}

void wa_mlow_decode_pulses(wa_rc *dec, int p2, int p3, int p4, int p6, int s1, wa_mlow_pulse_result *out)
{
    wa_cc_init();
    memset(out, 0, sizeof *out);
    if (p2 > 320) p2 = 320;

    int idx = p4 + s1;
    int bByte = mem8_static(0xe8990u + (uint32_t)(p6 * 3 + idx));
    int frameLen4k = bByte * p2 / 320;
    int subfrLen16 = frameLen4k / p3;
    int posPerSubfr = p2 / p3;

    // pulse count (NB config=0: triangular prior over [0, frameLen4k])
    int total = 0;
    if (p6 != 0) {
        // (LR path uses a CDF table from the seed; not on the active WB 1:1 path)
        total = 0;
    } else {
        uint32_t l = (uint32_t)frameLen4k;
        uint32_t limit = (uint32_t)frameLen4k + 1;
#define TRI(k) ((((k) + 2) * (l + 1) - ((((k) - 1) * ((k) + 131070u)) >> 1)) & 0xffffu)
        uint32_t ft = TRI(l);
        if (ft == 0) ft = 1;
        uint32_t val = wa_rc_decode(dec, ft);
        uint32_t prevCum = 0, k = 0;
        for (; k != limit; k++) {
            uint32_t cum = TRI(k);
            if (prevCum <= val && val < cum) { wa_rc_update(dec, prevCum, cum, ft); break; }
            prevCum = cum;
        }
#undef TRI
        total = (int)k;
    }

    int32_t split[8];
    if (decode_split(dec, total, subfrLen16, p3, split) != 0) return;
    for (int i = 0; i < p3 && i < 4; i++) out->subfr[i] = split[i];

    // magnitude run-length positions
    int posList[512], magList[512];
    int pulseIdx = -1;
    for (int subfr = 0; subfr < p3; subfr++) {
        int cnt = split[subfr];
        if (cnt <= 0) continue;
        int basePos = posPerSubfr * subfr;
        int runPos = basePos;
        int pos = posPerSubfr;
        int c = cnt;
        for (int k = 0; k < cnt; k++) {
            if (pos < 0) break;
            int oct = (pos + 7) / 8;
            size_t n;
            const uint16_t *cmf = wa_cc_runlen_cmf(oct, c, &n);
            if (!cmf) break;
            int start = wa_cc_runlen_max(oct) - pos;
            int m = wa_rc_decode_cdf_window(dec, cmf, n, start, pos + 1);
            if (m > 0 || k == 0) {
                if (pulseIdx + 1 >= 512) break;
                pulseIdx++;
                runPos += m;
                posList[pulseIdx] = runPos;
                magList[pulseIdx] = 1;
                pos -= m;
            } else if (pulseIdx >= 0) {
                magList[pulseIdx]++;
            }
            c--;
        }
    }
    int numPos = pulseIdx + 1;

    // signs: batched uniform bits (1 per position, 15 at a time)
    if (numPos > 0) {
        int p = 0;
        while (p <= pulseIdx) {
            int nbits = numPos - p;
            if (nbits >= 15) nbits = 15;
            if (nbits <= 0) break;
            uint32_t sym = wa_rc_decode_raw_symbol(dec, (uint32_t)nbits);
            uint32_t bitfield = sym << (uint32_t)(16 - nbits);
            int end = p + nbits;
            for (int q = p; q < end; q++) {
                int sign = (int)((bitfield >> 14) & 2) - 1;
                magList[q] *= sign;
                bitfield <<= 1;
            }
            p = end;
        }
    }

    for (int i = 0; i < numPos; i++) {
        int pp = posList[i];
        if (pp >= 0 && pp < p2) out->pulses[pp] = magList[i];
    }
}

extern const int wa_mlow_pulse_kat_len;
extern const int wa_mlow_pulse_kat_framelen[];
extern const int wa_mlow_pulse_kat_frameoff[];
extern const uint8_t wa_mlow_pulse_kat_frames[];
extern const int32_t wa_mlow_pulse_kat_subfr[][4];
extern const int wa_mlow_pulse_kat_npos[];
extern const int wa_mlow_pulse_kat_posoff[];
extern const int wa_mlow_pulse_kat_pos[];
extern const int wa_mlow_pulse_kat_val[];

int wa_mlow_pulse_selftest(void)
{
    for (int i = 0; i < wa_mlow_pulse_kat_len; i++) {
        const uint8_t *frame = &wa_mlow_pulse_kat_frames[wa_mlow_pulse_kat_frameoff[i]];
        int flen = wa_mlow_pulse_kat_framelen[i];
        wa_rc dec;
        wa_rc_init(&dec, frame + 1, (size_t)(flen - 1));
        wa_mlow_lsf_state st;
        memset(&st, 0, sizeof st);
        wa_mlow_lsf_indices lsf;
        wa_mlow_decode_lsf(&dec, 0, 0, 1, &st, &lsf);
        wa_mlow_pulse_result pr;
        wa_mlow_decode_pulses(&dec, 320, 4, 1, 0, lsf.stage1, &pr);
        int ok = dec.err == 0;
        for (int k = 0; k < 4 && ok; k++) if (pr.subfr[k] != wa_mlow_pulse_kat_subfr[i][k]) ok = 0;
        // reconstruct expected pulse vector from the nonzero list and compare all 320
        if (ok) {
            int32_t want[320];
            memset(want, 0, sizeof want);
            int off = wa_mlow_pulse_kat_posoff[i], np = wa_mlow_pulse_kat_npos[i];
            for (int j = 0; j < np; j++) want[wa_mlow_pulse_kat_pos[off + j]] = wa_mlow_pulse_kat_val[off + j];
            for (int k = 0; k < 320 && ok; k++) if (pr.pulses[k] != want[k]) ok = 0;
        }
        if (!ok) { wa_log("mlow pulse kat %d: subfr/pulses mismatch (err=%d)", i, dec.err); return -1; }
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
