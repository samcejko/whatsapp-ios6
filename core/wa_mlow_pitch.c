// MLow pitch / LTP decode (voiced internal frames). Two parts: the LTP gain+filter indices (Group C, served
// from the seed-built cc tables - wa_cc_acbgain_* / wa_cc_fcbgain_*), and the pitch lag/contour block, which
// replicates the reference's pointer chase through a carved heap window (Group D). That window is built from
// the pitch_seed ROM (wa_mlow_pitch_seed.c) exactly as meowcaller mem.go / pitch_seed.go do, so the absolute
// addresses the decode reads land unchanged. Ported from meowcaller pitch.go + mem.go + pitch_seed.go (MIT).
#include "wa_mlow.h"
#include "wa_mlow_cc.h"
#include "wa_mlow_rc.h"
#include "wa_util.h"

#include <string.h>

extern const uint8_t wa_mlow_pitch_seed[];
extern const int wa_mlow_pitch_seed_len;

// --- fixed WASM-build heap layout (mem.go) ---
#define MEM_GCLK        0xb9f9a8u
#define MEM_PCFG        (MEM_GCLK + 0x5704u)
#define MEM_R0_BASE     (MEM_PCFG + 0x1d38u)
#define MEM_LAGCDF      0xbaa7b0u
#define MEM_FRACBASE    0xbaa9beu
#define MEM_DELTACDF    0xbab13eu
#define MEM_CONTOURMAP  0xe7c10u
#define MEM_DELTABOUNDS 0xe7ef0u
#define MEM_HDR_UNUSED0 0xe7d20u
#define MEM_HDR_UNUSED2 0xe8096u
#define MEM_NUM_CONTOURS 217
#define MEM_NUM_BLOCKSEGS 217
#define MEM_NUM_BLOCKS    9

// region buffers (sized to the exact expanded layout)
static uint8_t g_r0[MEM_NUM_BLOCKSEGS * 68 + 4 + 32];          // 217 records (17 i32) + 187 gap + 8 hdr u32
static uint8_t g_lagcdf[(MEM_NUM_BLOCKSEGS + 1) * 2];          // dcmf_to_cmf(blockseg_idx) = 218 u16
static uint8_t g_fracbase[3 * 320 * 2];                        // 3 x 320 u16
static uint8_t g_deltacdf[MEM_NUM_BLOCKS * 10 * 2];            // 9 x 10 u16
static uint8_t g_contourmap[MEM_NUM_BLOCKSEGS];               // blocksegs2idx (217)
static uint8_t g_deltabounds[32];                             // firstblockRange pairs + {0,0}
static int g_deltabounds_n;

typedef struct { uint32_t base; const uint8_t *data; int len; } mem_region;
static mem_region g_regions[6];
static int g_nregions;
static int g_pitch_ready;

// --- pitch seed protobuf (all wiretype-2 byte fields) ---
typedef struct { const uint8_t *bytes; size_t len; int present; } ps_field;

static uint64_t ps_rv(const uint8_t *b, size_t n, size_t *i)
{
    uint64_t v = 0; int s = 0;
    while (*i < n) { uint8_t c = b[(*i)++]; v |= (uint64_t)(c & 0x7f) << s; if (!(c & 0x80)) break; s += 7; }
    return v;
}

static void ps_parse(ps_field *f /* [8] */)
{
    memset(f, 0, sizeof(ps_field) * 8);
    const uint8_t *b = wa_mlow_pitch_seed;
    size_t n = (size_t)wa_mlow_pitch_seed_len, i = 0;
    while (i < n) {
        uint64_t key = ps_rv(b, n, &i);
        int field = (int)(key >> 3), wire = (int)(key & 7);
        if (wire != 2) break;   // seeds are all length-delimited
        size_t ln = (size_t)ps_rv(b, n, &i);
        if (i + ln > n) break;
        if (field >= 1 && field <= 7) { f[field].bytes = b + i; f[field].len = ln; f[field].present = 1; }
        i += ln;
    }
}

// INTEGER pitch dcmf->cmf (int64 cumulative; result fits in u16). out holds n+1 entries.
static void pitch_dcmf_to_cmf(const uint8_t *dcmf, int n, uint16_t *out)
{
    int64_t sum = 0;
    for (int i = 0; i < n; i++) {
        int32_t t = (int32_t)dcmf[i] + 1;
        t *= t;
        if (t > 65535) t = 65535;
        out[i + 1] = (uint16_t)t;
        sum += t;
    }
    if (sum == 0) sum = 1;
    out[0] = 0;
    for (int i = 1; i <= n; i++) {
        int64_t prev = out[i - 1];
        int64_t add = (int64_t)out[i] * (32767 - (int64_t)n) / sum + 1;
        out[i] = (uint16_t)(prev + add);
    }
}

// range-decode a uniform symbol in [0,n)
static uint32_t ps_uniform(wa_rc *d, uint32_t n)
{
    uint32_t v = wa_rc_decode(d, n);
    wa_rc_update(d, v, v + 1, n);
    return v;
}

static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// Build the Group-D heap window from the pitch seed (port of mem.go buildSmplMemFromSeed + pitch_seed.go
// buildContourWindow). Fills the region buffers + the region table.
static int build_contour_window(void)
{
    ps_field f[8];
    ps_parse(f);
    if (!f[1].present || !f[2].present || !f[4].present || !f[5].present || !f[6].present || !f[7].present)
        return -1;

    // (1) range-decode 217 blocksegs from the bitstream (field 1) and lay out r0.
    wa_rc dec;
    wa_rc_init(&dec, f[1].bytes, f[1].len);
    int pos = 0;
    for (int rec = 0; rec < MEM_NUM_BLOCKSEGS; rec++) {
        int length = (int)ps_uniform(&dec, 6) + 1;
        int blocks[8] = { 0 }, seglens[8] = { 0 };
        for (int j = 0; j < length; j++) {
            int bk = (int)ps_uniform(&dec, 9);
            int sl = (int)ps_uniform(&dec, 4) + 1;
            if (j < 8) { blocks[j] = bk; seglens[j] = sl; }
        }
        for (int i = 0; i < 8; i++) { le32(&g_r0[pos], (uint32_t)blocks[i]); pos += 4; }
        for (int i = 0; i < 8; i++) { le32(&g_r0[pos], (uint32_t)seglens[i]); pos += 4; }
        le32(&g_r0[pos], (uint32_t)length); pos += 4;
    }
    le32(&g_r0[pos], 187); pos += 4;   // NUM_BLOCKTRACKS gap
    uint32_t hdr[8] = { MEM_NUM_CONTOURS, MEM_CONTOURMAP, MEM_LAGCDF, MEM_FRACBASE,
                        MEM_HDR_UNUSED0, MEM_DELTABOUNDS, MEM_HDR_UNUSED2, MEM_DELTACDF };
    for (int i = 0; i < 8; i++) { le32(&g_r0[pos], hdr[i]); pos += 4; }

    // (2) lagCdf = pitch_dcmf_to_cmf(blockseg_idx_dcmf = field 5), stored as u16 LE.
    {
        int n = (int)f[5].len;                      // 217
        uint16_t cmf[MEM_NUM_BLOCKSEGS + 2];
        if (n + 1 > (int)(sizeof(g_lagcdf) / 2)) return -1;
        pitch_dcmf_to_cmf(f[5].bytes, n, cmf);
        for (int i = 0; i <= n; i++) le16(&g_lagcdf[i * 2], cmf[i]);
    }
    // (3) fracCmfs = 3 x pitch_dcmf_to_cmf(deltaLagDcmfs[i:i+319]) (field 6), each 320 entries.
    {
        int off = 0, step = 319;
        for (int c = 0; c + step <= (int)f[6].len; c += step) {
            uint16_t cmf[321];
            pitch_dcmf_to_cmf(f[6].bytes + c, step, cmf);
            for (int i = 0; i <= step; i++) { le16(&g_fracbase[off], cmf[i]); off += 2; }
        }
    }
    // (4) deltaCmfs = 9 x pitch_dcmf_to_cmf(blockTransitionDcmf[i:i+9]) (field 7), each 10 entries.
    {
        int off = 0, step = MEM_NUM_BLOCKS;
        for (int c = 0; c + step <= (int)f[7].len; c += step) {
            uint16_t cmf[MEM_NUM_BLOCKS + 1];
            pitch_dcmf_to_cmf(f[7].bytes + c, step, cmf);
            for (int i = 0; i <= step; i++) { le16(&g_deltacdf[off], cmf[i]); off += 2; }
        }
    }
    // (5) contourMap = blocksegs2idx (field 2), copied verbatim.
    {
        int n = (int)f[2].len; if (n > (int)sizeof(g_contourmap)) n = (int)sizeof(g_contourmap);
        memcpy(g_contourmap, f[2].bytes, (size_t)n);
    }
    // (6) deltaBounds = firstblockRange pairs (field 4) as bytes + {0,0}.
    {
        int n = (int)f[4].len; n &= ~1;              // whole pairs
        if (n + 2 > (int)sizeof(g_deltabounds)) n = (int)sizeof(g_deltabounds) - 2;
        memcpy(g_deltabounds, f[4].bytes, (size_t)n);
        g_deltabounds[n] = 0; g_deltabounds[n + 1] = 0;
        g_deltabounds_n = n + 2;
    }

    g_nregions = 0;
    g_regions[g_nregions++] = (mem_region){ MEM_R0_BASE, g_r0, (int)sizeof(g_r0) };
    g_regions[g_nregions++] = (mem_region){ MEM_LAGCDF, g_lagcdf, (int)sizeof(g_lagcdf) };
    g_regions[g_nregions++] = (mem_region){ MEM_FRACBASE, g_fracbase, (int)sizeof(g_fracbase) };
    g_regions[g_nregions++] = (mem_region){ MEM_DELTACDF, g_deltacdf, (int)sizeof(g_deltacdf) };
    g_regions[g_nregions++] = (mem_region){ MEM_CONTOURMAP, g_contourmap, (int)sizeof(g_contourmap) };
    g_regions[g_nregions++] = (mem_region){ MEM_DELTABOUNDS, g_deltabounds, g_deltabounds_n };
    return 0;
}

int wa_mlow_pitch_init(void)
{
    if (g_pitch_ready) return 0;
    if (wa_cc_seed_init() != 0) return -1;
    if (build_contour_window() != 0) return -1;
    g_pitch_ready = 1;
    return 0;
}

// --- region reads (U8/U16/U32/I32/CDFAt), zero outside every region ---
static const uint8_t *mem_at(uint32_t addr, int n, int *off)
{
    for (int r = 0; r < g_nregions; r++) {
        mem_region *m = &g_regions[r];
        if (addr >= m->base && (int)(addr - m->base) + n <= m->len) { *off = (int)(addr - m->base); return m->data; }
    }
    return NULL;
}
static uint8_t mem_u8(uint32_t addr) { int o; const uint8_t *d = mem_at(addr, 1, &o); return d ? d[o] : 0; }
static uint16_t mem_u16(uint32_t addr) { int o; const uint8_t *d = mem_at(addr, 2, &o); return d ? (uint16_t)(d[o] | (d[o + 1] << 8)) : 0; }
static uint32_t mem_u32(uint32_t addr) { int o; const uint8_t *d = mem_at(addr, 4, &o); return d ? (uint32_t)(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24)) : 0; }
static int32_t mem_i32(uint32_t addr) { return (int32_t)mem_u32(addr); }

// Materialize the n-entry u16 CDF at addr into out (caller-provided), 0 outside the window.
static void mem_cdf_at(uint32_t addr, int n, uint16_t *out)
{
    for (int i = 0; i < n; i++) out[i] = mem_u16(addr + (uint32_t)i * 2);
}

// Decode the LTP gains+filters and pitch lag/contour for one internal frame. p3 = num subframes,
// p6 = config (0 = HR / active WB), subfr = per-subframe pulse counts. Mirrors pitch.go DecodeSmplPitch.
void wa_mlow_decode_pitch(wa_rc *dec, wa_mlow_lsf_state *st, int p3, int p6, const int32_t subfr[4],
                          wa_mlow_pitch_result *res)
{
    memset(res, 0, sizeof(*res));
    for (int i = 0; i < 4; i++) res->filt_idx[i] = -1;

    // --- LTP gains loop (Group C) ---
    int32_t gain_accum = 0;
    int take = p3 > 4 ? 4 : p3;
    for (int sf = 0; sf < take; sf++) {
        int32_t cnt = subfr[sf];
        size_t gl;
        const uint16_t *grow = (p6 != 0) ? NULL : wa_cc_acbgain_row(st->prev_gain_idx, &gl);
        if (!grow) { dec->err = 1; return; }       // LR path not used by the active WB KAT
        int32_t gi = wa_rc_decode_cdf(dec, grow, gl);
        res->gain_idx[sf] = gi;
        st->prev_gain_idx = gi;

        int32_t w0, w2;
        wa_cc_acbgain_weights(gi, &w0, &w2);
        gain_accum += w0 + 2 * w2;

        if (cnt > 0) {
            size_t fl;
            const uint16_t *fcdf = (st->prev_filt_idx == -1) ? wa_cc_fcbgain_v(&fl)
                                                             : wa_cc_fcbgain_v_delta(st->prev_filt_idx, &fl);
            if (!fcdf) { dec->err = 1; return; }
            int32_t fi = wa_rc_decode_cdf(dec, fcdf, fl);
            res->filt_idx[sf] = fi;
            st->prev_filt_idx = fi;
        }
    }
    int32_t avg_gain = gain_accum / p3;

    // --- lag block (Group D pointer chase) ---
    uint32_t pcfg = MEM_GCLK + 0x5704u;
    int32_t num_contours = (int32_t)mem_u32(pcfg + 22240);
    uint32_t lag_cdf = mem_u32(pcfg + 22248);
    uint32_t contour_map = mem_u32(pcfg + 22244);
    uint32_t frac_base = mem_u32(pcfg + 22252);
    uint32_t delta_cdf = mem_u32(pcfg + 22268);

    uint16_t cdf[256];
    int32_t lag;
    if (st->prev_lag < 0) {
        int32_t cnt = num_contours + 1;
        if (cnt < 0) cnt = 0;
        if (cnt > 256) cnt = 256;
        mem_cdf_at(lag_cdf, (int)cnt, cdf);
        lag = wa_rc_decode_cdf(dec, cdf, (size_t)cnt);
    } else {
        mem_cdf_at(delta_cdf + (uint32_t)st->prev_lag * 20, 10, cdf);
        int32_t di = wa_rc_decode_cdf(dec, cdf, 10);
        int32_t lo = (int32_t)mem_u8(MEM_DELTABOUNDS + (uint32_t)di * 2);
        int32_t hi = (int32_t)mem_u8(MEM_DELTABOUNDS + (uint32_t)di * 2 + 1);
        int32_t rN = (hi - lo) + 2;
        if (rN < 2) { res->lag = -1; return; }
        if (rN > 256) rN = 256;
        mem_cdf_at(lag_cdf + (uint32_t)lo * 2, (int)rN, cdf);
        int32_t sym = wa_rc_decode_cdf(dec, cdf, (size_t)rN);
        lag = sym + lo;
    }

    // contour-map search: index i where contour_map[i] == lag+1
    int32_t target = lag + 1;
    int32_t contour = -1;
    for (int32_t i = 0; i < 217; i++) {
        if ((int32_t)mem_u8(contour_map + (uint32_t)i) == target) { contour = i; break; }
    }
    res->lag = lag;
    res->contour = contour;
    if (contour < 0 || contour >= num_contours) return;

    uint32_t ctr_base = pcfg + (uint32_t)contour * 0x44u;
    int32_t base_lag = mem_i32(ctr_base + 0x1d38);

    int32_t cur_lag2 = base_lag;
    int read_fine = 1;
    if (st->prev_lag >= 0) {
        int32_t delta = base_lag - st->prev_lag;
        if (delta >= -1 && delta < 3) read_fine = 0;
    }
    int32_t subfr_w = 0;
    if (read_fine) {
        int32_t sym = wa_rc_decode_64_fine(dec);
        cur_lag2 = (base_lag << 6) + sym;
        st->prev_frac_lag = cur_lag2;
        st->prev_lag = base_lag;
        int32_t seg_len0 = mem_i32(ctr_base + 0x1d58);
        for (int32_t i = 0; i < seg_len0; i++) {
            if (subfr_w < 4) res->int_lag_q6[subfr_w] = cur_lag2;
            if (subfr_w < 8) res->block_lags[subfr_w] = cur_lag2;
            subfr_w++;
        }
        if (subfr_w < 4) res->int_lag_q6[subfr_w] = cur_lag2;   // trailing write, not incremented
        if (subfr_w < 8) res->block_lags[subfr_w] = cur_lag2;
    }

    // fractional per-segment loop
    int32_t cnt2 = mem_i32(ctr_base + 0x1d78);
    int32_t seg_sel = 0;
    if (avg_gain >= 10007) seg_sel = (avg_gain < 14085) ? 1 : 2;
    uint32_t frac_seg_base = frac_base + (uint32_t)seg_sel * 0x280u;
    int32_t l3 = st->prev_frac_lag;
    int32_t l2 = cur_lag2;
    int32_t start_seg = read_fine ? 1 : 0;
    res->num_seg = cnt2;
    for (int32_t seg = start_seg; seg < cnt2; seg++) {
        int32_t seg_lag = mem_i32(ctr_base + 0x1d38 + (uint32_t)seg * 4);
        int32_t nl2 = ((l2 << 6) - l3) + ((seg_lag - l2) << 6);
        uint32_t off = frac_seg_base + (uint32_t)(nl2 * 2) + 0xfe;
        mem_cdf_at(off, 65, cdf);
        int32_t sym = wa_rc_decode_cdf(dec, cdf, 65);
        l3 = sym + st->prev_frac_lag + nl2;
        if (seg < 8) res->sample_lag_q6[seg] = l3;
        int32_t seg_len = mem_i32(ctr_base + 0x1d58 + (uint32_t)seg * 4);
        for (int32_t i = 0; i < seg_len; i++) {
            if (subfr_w < 4) res->int_lag_q6[subfr_w] = l3;
            if (subfr_w < 8) res->block_lags[subfr_w] = l3;
            subfr_w++;
        }
        l2 = seg_lag;
        st->prev_frac_lag = l3;
        st->prev_lag = seg_lag;
    }
    res->num_subfr = subfr_w;
}

// --- KAT (meowcaller pitch_vectors.json): LSF(0)->pulses(0)->pitch(0) vs lag/contour/gain_idx/filt_idx/int_lag_q6 ---
extern const int wa_mlow_pitch_kat_len;
extern const int wa_mlow_pitch_kat_framelen[];
extern const int wa_mlow_pitch_kat_frameoff[];
extern const uint8_t wa_mlow_pitch_kat_frames[];
extern const int32_t wa_mlow_pitch_kat_lag[];
extern const int32_t wa_mlow_pitch_kat_contour[];
extern const int32_t wa_mlow_pitch_kat_gainidx[][4];
extern const int32_t wa_mlow_pitch_kat_filtidx[][4];
extern const int32_t wa_mlow_pitch_kat_ilagq6[][4];

int wa_mlow_pitch_selftest(void)
{
    if (wa_cc_init() != 0 || wa_mlow_pitch_init() != 0) { wa_log("mlow pitch: table init failed"); return -1; }

    for (int i = 0; i < wa_mlow_pitch_kat_len; i++) {
        const uint8_t *frame = &wa_mlow_pitch_kat_frames[wa_mlow_pitch_kat_frameoff[i]];
        int flen = wa_mlow_pitch_kat_framelen[i];
        wa_rc dec;
        wa_rc_init(&dec, frame + 1, (size_t)(flen - 1));
        wa_mlow_lsf_state st;
        memset(&st, 0, sizeof st);
        wa_mlow_lsf_indices lsf;
        wa_mlow_decode_lsf(&dec, 0, 0, 1, &st, &lsf);
        wa_mlow_pulse_result pr;
        wa_mlow_decode_pulses(&dec, 320, 4, 1, 0, lsf.stage1, &pr);
        wa_mlow_pitch_result pit;
        wa_mlow_decode_pitch(&dec, &st, 4, 0, pr.subfr, &pit);

        int ok = dec.err == 0 && pit.lag == wa_mlow_pitch_kat_lag[i] && pit.contour == wa_mlow_pitch_kat_contour[i];
        for (int k = 0; k < 4 && ok; k++) if (pit.gain_idx[k] != wa_mlow_pitch_kat_gainidx[i][k]) ok = 0;
        for (int k = 0; k < 4 && ok; k++) if (pit.filt_idx[k] != wa_mlow_pitch_kat_filtidx[i][k]) ok = 0;
        for (int k = 0; k < 4 && ok; k++) if (pit.int_lag_q6[k] != wa_mlow_pitch_kat_ilagq6[i][k]) ok = 0;
        if (!ok) {
            wa_log("mlow pitch kat %d: mismatch (err=%d) lag=%d/%d contour=%d/%d", i, dec.err,
                   pit.lag, wa_mlow_pitch_kat_lag[i], pit.contour, wa_mlow_pitch_kat_contour[i]);
            wa_log("  gain %d,%d,%d,%d filt %d,%d,%d,%d ilag %d,%d,%d,%d",
                   pit.gain_idx[0], pit.gain_idx[1], pit.gain_idx[2], pit.gain_idx[3],
                   pit.filt_idx[0], pit.filt_idx[1], pit.filt_idx[2], pit.filt_idx[3],
                   pit.int_lag_q6[0], pit.int_lag_q6[1], pit.int_lag_q6[2], pit.int_lag_q6[3]);
            return -1;
        }
    }
    return 0;
}
