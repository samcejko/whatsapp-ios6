// MLow per-subframe gains + energy-residual decode (the "GAINS" block, used for unvoiced internal frames -
// LSF stage-1 selector 0). The nrgres/gains CDFs come from the seed-built cc tables: we parse the inflated
// cc_seed protobuf ROM (wa_mlow_cc_seed.c) and expand its DCMF arrays with the integer celp dcmf->cmf
// transform, mirroring meowcaller cc_tables.go build() + smpl_gains.go DecodeSmplGains (MIT).
#include "wa_mlow.h"
#include "wa_mlow_cc.h"
#include "wa_mlow_rc.h"
#include "wa_util.h"

#include <string.h>

// --- embedded inflated cc_seed protobuf ROM (tools/gen-cc-seed.ps1) ---
extern const uint8_t wa_mlow_cc_seed[];
extern const int wa_mlow_cc_seed_len;

// Two-subframe residual-energy DCMFs are compiled constants in the reference (not in the seed ROM); the
// low-rate operating point uses them. Verbatim from meowcaller cc_tables.go.
static const uint8_t cc_nrgres_gain2_dcmf[] = {
    139, 194, 196, 200, 209, 225, 239, 243, 246, 246, 253, 255,
    253, 249, 244, 239, 233, 231, 230, 228, 219, 216, 214, 214,
    213, 211, 214, 215, 220, 223, 230, 234, 237, 240, 242, 244,
    245, 245, 246, 242, 237, 230, 220, 210, 190, 170, 148, 125,
    104, 83, 64, 49, 36, 22, 14, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8,
};
static const uint8_t cc_nrgres_shape2_dcmf[] = {
    47, 52, 137, 58, 61, 21, 73, 240, 55, 95, 255,
    68, 83, 108, 48, 50, 49, 45, 35, 155, 31, 41,
};

#define CC_FCBG_OFFSET_STEPS   176   // DCMF bytes per fcbgOffset chunk
#define CC_FCBG_OFFSET_BUCKETS 4
#define CC_FCBG_OFFSET_CHUNKS  12    // 2112 / 176
#define CC_FCBG_WINDOW         92
#define CC_GAIN_RECON_N        1808  // 3616 bytes / 2

// expanded tables
static uint16_t g_nrgres_gain2[sizeof(cc_nrgres_gain2_dcmf) + 1];
static uint16_t g_nrgres_shape2[sizeof(cc_nrgres_shape2_dcmf) + 1];
static uint16_t g_nrgres_gain4[85];   // seed field 1 = 84 bytes
static int g_nrgres_gain4_n;
static uint16_t g_nrgres_shape4[99];  // seed field 2 = 98 bytes
static int g_nrgres_shape4_n;
static uint16_t g_fcbg_offset[CC_FCBG_OFFSET_CHUNKS][CC_FCBG_OFFSET_STEPS + 1];
static int16_t g_gain_recon[CC_GAIN_RECON_N];
static int g_gain_recon_n;
static uint32_t g_gain_recon_base;
static int g_gains_ready;

// INTEGER, bit-exact dcmf->cmf (truncating-int normalize). out holds n+1 entries. uint16 wrap is intentional.
static void celp_dcmf_to_cmf(const uint8_t *dcmf, int n, uint16_t *cmf)
{
    int32_t sum = 0;
    for (int k = 0; k < n; k++) {
        int32_t tmp = (int32_t)dcmf[k] + 1;
        tmp *= tmp;
        if (tmp > 65535) tmp = 65535;
        cmf[k + 1] = (uint16_t)tmp;
        sum += tmp;
    }
    if (sum == 0) sum = 1;
    cmf[0] = 0;
    for (int k = 1; k < n + 1; k++) {
        cmf[k] = (uint16_t)((uint16_t)cmf[k - 1] + (uint16_t)(((int32_t)cmf[k] * (32767 - n)) / sum) + 1);
    }
}

// one decoded protobuf field (wiretype 0 varint or 2 bytes), pointing into the embedded blob.
typedef struct { int wire; uint64_t varint; const uint8_t *bytes; size_t len; int present; } cc_field;

static uint64_t pb_rv(const uint8_t *b, size_t n, size_t *i)
{
    uint64_t v = 0; int s = 0;
    while (*i < n) {
        uint8_t c = b[(*i)++];
        v |= (uint64_t)(c & 0x7f) << s;
        if (!(c & 0x80)) break;
        s += 7;
    }
    return v;
}

// Parse the top-level seed protobuf into fields[1..12].
static void cc_parse_seed(cc_field *f /* [13] */)
{
    memset(f, 0, sizeof(cc_field) * 13);
    const uint8_t *b = wa_mlow_cc_seed;
    size_t n = (size_t)wa_mlow_cc_seed_len, i = 0;
    while (i < n) {
        uint64_t key = pb_rv(b, n, &i);
        int field = (int)(key >> 3), wire = (int)(key & 7);
        if (wire == 0) {
            uint64_t v = pb_rv(b, n, &i);
            if (field >= 1 && field <= 12) { f[field].wire = 0; f[field].varint = v; f[field].present = 1; }
        } else if (wire == 2) {
            size_t ln = (size_t)pb_rv(b, n, &i);
            if (i + ln > n) break;
            if (field >= 1 && field <= 12) { f[field].wire = 2; f[field].bytes = b + i; f[field].len = ln; f[field].present = 1; }
            i += ln;
        } else {
            break;
        }
    }
}

int wa_cc_seed_init(void)
{
    if (g_gains_ready) return 0;
    cc_field f[13];
    cc_parse_seed(f);
    if (!f[1].present || !f[2].present || !f[3].present || !f[8].present || !f[9].present) return -1;

    celp_dcmf_to_cmf(cc_nrgres_gain2_dcmf, (int)sizeof(cc_nrgres_gain2_dcmf), g_nrgres_gain2);
    celp_dcmf_to_cmf(cc_nrgres_shape2_dcmf, (int)sizeof(cc_nrgres_shape2_dcmf), g_nrgres_shape2);

    g_nrgres_gain4_n = (int)f[1].len + 1;
    if (g_nrgres_gain4_n > (int)(sizeof(g_nrgres_gain4) / sizeof(g_nrgres_gain4[0]))) return -1;
    celp_dcmf_to_cmf(f[1].bytes, (int)f[1].len, g_nrgres_gain4);

    g_nrgres_shape4_n = (int)f[2].len + 1;
    if (g_nrgres_shape4_n > (int)(sizeof(g_nrgres_shape4) / sizeof(g_nrgres_shape4[0]))) return -1;
    celp_dcmf_to_cmf(f[2].bytes, (int)f[2].len, g_nrgres_shape4);

    if (f[3].len != (size_t)CC_FCBG_OFFSET_STEPS * CC_FCBG_OFFSET_CHUNKS) return -1;
    for (int c = 0; c < CC_FCBG_OFFSET_CHUNKS; c++)
        celp_dcmf_to_cmf(f[3].bytes + c * CC_FCBG_OFFSET_STEPS, CC_FCBG_OFFSET_STEPS, g_fcbg_offset[c]);

    g_gain_recon_base = (uint32_t)f[8].varint;
    g_gain_recon_n = (int)(f[9].len / 2);
    if (g_gain_recon_n > CC_GAIN_RECON_N) g_gain_recon_n = CC_GAIN_RECON_N;
    for (int k = 0; k < g_gain_recon_n; k++)
        g_gain_recon[k] = (int16_t)((uint16_t)f[9].bytes[2 * k] | ((uint16_t)f[9].bytes[2 * k + 1] << 8));

    g_gains_ready = 1;
    return 0;
}

const uint16_t *wa_cc_nrgres_gain(int p3, size_t *len)
{
    if (p3 == 2) { *len = sizeof(cc_nrgres_gain2_dcmf) + 1; return g_nrgres_gain2; }
    *len = (size_t)g_nrgres_gain4_n; return g_nrgres_gain4;
}

const uint16_t *wa_cc_nrgres_shape(int p3, size_t *len)
{
    if (p3 == 2) { *len = sizeof(cc_nrgres_shape2_dcmf) + 1; return g_nrgres_shape2; }
    *len = (size_t)g_nrgres_shape4_n; return g_nrgres_shape4;
}

// gainReconAt(addr): the seed stores a flat int16 array based at g_gain_recon_base; even byte offsets only.
static int32_t cc_gain_recon_at(uint32_t addr)
{
    int off = (int)(addr - g_gain_recon_base);
    if (off >= 0 && (off % 2) == 0 && off / 2 < g_gain_recon_n) return (int32_t)g_gain_recon[off / 2];
    return 0;
}

int32_t wa_cc_nrg_step(int cfg) { return cc_gain_recon_at(g_gain_recon_base + (uint32_t)cfg * 2); }

int32_t wa_cc_gain_recon(int p4, int32_t idx)
{
    uint32_t base = p4 ? 0xf35f0u : 0xf3970u;
    return cc_gain_recon_at(base + (uint32_t)idx * 2);
}

const uint16_t *wa_cc_fcbg_offset(int table_ix, int bucket, int min_offset)
{
    int idx = table_ix * CC_FCBG_OFFSET_BUCKETS + bucket;
    if (idx < 0 || idx >= CC_FCBG_OFFSET_CHUNKS) return NULL;
    if (min_offset < 0 || min_offset + CC_FCBG_WINDOW > CC_FCBG_OFFSET_STEPS + 1) return NULL;
    return &g_fcbg_offset[idx][min_offset];
}

// Decode gains+nrgres. subfr[] are the per-subframe pulse counts from the pulse stage; p3 is the subframe
// geometry (2 or 4). Mirrors meowcaller gains.go DecodeSmplGains.
void wa_mlow_decode_gains(wa_rc *dec, int p3, const int32_t subfr[4], wa_mlow_gain_result *out)
{
    memset(out, 0, sizeof(*out));
    size_t gn, sn;
    const uint16_t *gain_cdf = wa_cc_nrgres_gain(p3, &gn);
    const uint16_t *shape_cdf = wa_cc_nrgres_shape(p3, &sn);
    int32_t gain_main = wa_rc_decode_cdf(dec, gain_cdf, gn);
    int32_t gain_delta = wa_rc_decode_cdf(dec, shape_cdf, sn);
    out->gain_main = gain_main;
    out->gain_delta = gain_delta;

    int32_t cfg_sel = (p3 == 2) ? 1 : 2;
    int32_t off6 = (int32_t)p3 * gain_delta;
    int32_t base7 = gain_main * wa_cc_nrg_step(cfg_sel) - 0x154000;
    int take = p3 > 4 ? 4 : p3;
    for (int sf = 0; sf < take; sf++) {
        int32_t cbv = wa_cc_gain_recon(p3 == 4, (int32_t)sf + off6);
        out->gain_q[sf] = base7 + (cbv << 4);
    }

    for (int sf = 0; sf < take; sf++) {
        int32_t cnt = subfr[sf];
        if (cnt <= 0) continue;
        int32_t bucket = (cnt >= 30) ? 3 : ((cnt & 0xffff) / 10);
        int32_t g = (out->gain_q[sf] + 8192) >> 14;
        if (g < -85) g = -85;
        int32_t neg_part = (g >> 31) & g;
        int min_offset = (int)(-neg_part);
        const uint16_t *w = wa_cc_fcbg_offset((int)cfg_sel, (int)bucket, min_offset);
        if (!w) { dec->err = 1; continue; }
        out->nrg_res[sf] = wa_rc_decode_cdf(dec, w, CC_FCBG_WINDOW);
    }
}

// --- KAT (meowcaller gains_vectors.json): replay LSF(0)->pulses(0)->gains and compare gain_q[] + nrg_res[] ---
extern const int wa_mlow_gains_kat_len;
extern const int wa_mlow_gains_kat_framelen[];
extern const int wa_mlow_gains_kat_frameoff[];
extern const uint8_t wa_mlow_gains_kat_frames[];
extern const int32_t wa_mlow_gains_kat_gainq[][4];
extern const int32_t wa_mlow_gains_kat_nrgres[][4];

int wa_mlow_gains_selftest(void)
{
    if (wa_cc_init() != 0 || wa_cc_seed_init() != 0) { wa_log("mlow gains: table init failed"); return -1; }

    for (int i = 0; i < wa_mlow_gains_kat_len; i++) {
        const uint8_t *frame = &wa_mlow_gains_kat_frames[wa_mlow_gains_kat_frameoff[i]];
        int flen = wa_mlow_gains_kat_framelen[i];
        wa_rc dec;
        wa_rc_init(&dec, frame + 1, (size_t)(flen - 1));
        wa_mlow_lsf_state st;
        memset(&st, 0, sizeof st);
        wa_mlow_lsf_indices lsf;
        wa_mlow_decode_lsf(&dec, 0, 0, 1, &st, &lsf);
        wa_mlow_pulse_result pr;
        wa_mlow_decode_pulses(&dec, 320, 4, 1, 0, lsf.stage1, &pr);
        wa_mlow_gain_result g;
        wa_mlow_decode_gains(&dec, 4, pr.subfr, &g);
        int ok = dec.err == 0;
        for (int k = 0; k < 4 && ok; k++) if (g.gain_q[k] != wa_mlow_gains_kat_gainq[i][k]) ok = 0;
        for (int k = 0; k < 4 && ok; k++) if (g.nrg_res[k] != wa_mlow_gains_kat_nrgres[i][k]) ok = 0;
        if (!ok) {
            wa_log("mlow gains kat %d: mismatch (err=%d) gainq %d,%d,%d,%d nrg %d,%d,%d,%d", i, dec.err,
                   g.gain_q[0], g.gain_q[1], g.gain_q[2], g.gain_q[3],
                   g.nrg_res[0], g.nrg_res[1], g.nrg_res[2], g.nrg_res[3]);
            return -1;
        }
    }

    // Two-subframe gain reconstruction (reference TestDecodeSmplGainsTwoSubframes): main=30, delta=10, p3=2.
    // This exercises NrgStep(1) + GainRecon(false,..) without an encoder; expected {-800294, -794406}.
    {
        int32_t off6 = 2 * 10;
        int32_t base7 = 30 * wa_cc_nrg_step(1) - 0x154000;
        int32_t q0 = base7 + (wa_cc_gain_recon(0, 0 + off6) << 4);
        int32_t q1 = base7 + (wa_cc_gain_recon(0, 1 + off6) << 4);
        if (q0 != -800294 || q1 != -794406) {
            wa_log("mlow gains 2sf recon: got {%d,%d} want {-800294,-794406}", q0, q1);
            return -1;
        }
    }
    return 0;
}
