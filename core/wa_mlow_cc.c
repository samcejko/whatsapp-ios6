#include "wa_mlow_cc.h"
#include "wa_util.h"

#include <stdlib.h>
#include <string.h>

// --- SILK fixed-point primitives (ported from meowcaller cc_tables.go / whatsapp-rust smpl_cc_tables.rs) ---

#define CC_LOG2_EXP1_Q15 47274
#define CC_LOG22PI_Q14 43442
#define CC_ONE_Q31 ((int64_t)1 << 31)

static int32_t cc_smulbb(int32_t a, int32_t b) { return (int32_t)(int16_t)a * (int32_t)(int16_t)b; }
static int32_t cc_smlawb(int32_t a, int32_t b, int32_t c) { return (int32_t)((int64_t)a + (((int64_t)b * (int64_t)(int16_t)c) >> 16)); }

static int clz32(uint32_t x)
{
    int n = 0;
    if (x == 0) return 32;
    while (!(x & 0x80000000u)) { n++; x <<= 1; }
    return n;
}

static void cc_clz_frac(int32_t in, int32_t *lz, int32_t *frac)
{
    uint32_t u = (uint32_t)in;
    int l = clz32(u);
    int r = (int)((24 - l) & 31);
    uint32_t rot = (u >> r) | (u << ((32 - r) & 31));   // rotate_left(u, -r) == rotate_right(u, r)
    *lz = l;
    *frac = (int32_t)(rot & 0x7f);
}

static int32_t cc_lin2log(int32_t in_lin)
{
    int32_t lz, frac;
    cc_clz_frac(in_lin, &lz, &frac);
    return cc_smlawb(frac, frac * (128 - frac), 179) + ((31 - lz) << 7);
}

static int32_t cc_log2lin(int32_t in_log_q7)
{
    if (in_log_q7 < 0) return 0;
    if (in_log_q7 >= 3967) return 0x7fffffff;
    int32_t out = (int32_t)1 << (uint32_t)(in_log_q7 >> 7);
    int32_t frac = in_log_q7 & 0x7f;
    int32_t inner = cc_smlawb(frac, cc_smulbb(frac, 128 - frac), -174);
    if (in_log_q7 < 2048) out += (out * inner) >> 7;
    else out += (out >> 7) * inner;
    return out;
}

static int32_t cc_sigm_q15(int32_t in_q5)
{
    static const int32_t slope[6] = { 237, 153, 73, 30, 12, 7 };
    static const int32_t pos[6] = { 16384, 23955, 28861, 31213, 32178, 32548 };
    static const int32_t neg[6] = { 16384, 8812, 3906, 1554, 589, 219 };
    if (in_q5 < 0) {
        int32_t v = -in_q5;
        if (v >= 6 * 32) return 0;
        int32_t ind = v >> 5;
        return neg[ind] - cc_smulbb(slope[ind], v & 0x1f);
    }
    if (in_q5 >= 6 * 32) return 32767;
    int32_t ind = in_q5 >> 5;
    return pos[ind] + cc_smulbb(slope[ind], in_q5 & 0x1f);
}

// smpl_pdf_to_CMF (maxval==-1 path): truncating-int normalize into a (n+1)-entry u16 CMF. out must hold n+1.
static void pdf_to_cmf(const int32_t *pdf, int n, uint16_t *out)
{
    const int64_t maxval = 32767;
    int64_t sump = 0;
    for (int i = 0; i < n; i++) sump += pdf[i];
    if (sump == 0) sump = 1;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        int64_t p = (int64_t)pdf[i] * (maxval - n) / sump + 1;
        out[i + 1] = (uint16_t)((int32_t)out[i] + (int32_t)p);
    }
}

static int32_t cc_stirling(int32_t n)
{
    if (n == 0) return 0;
    int32_t ret = ((n << 1) + 1) * (cc_lin2log(n) << 7);
    ret -= (int32_t)CC_LOG2_EXP1_Q15 * n;
    ret += CC_LOG22PI_Q14;
    return ret + CC_LOG2_EXP1_Q15 / (12 * n);
}

static int32_t cc_prob_split_fast(int32_t k, int32_t n)
{
    int32_t tmp = cc_stirling(n) - cc_stirling(k) - cc_stirling(n - k) - n * (1 << 15);
    if (tmp == 0) return 1 << 30;
    int32_t ret = cc_log2lin((-tmp) >> 8);
    if (ret == 0) return 1 << 30;
    return (1 << 30) / ret;
}

// --- the tables ---

typedef struct { uint16_t *data; int len; } cc_cmf;
static cc_cmf g_split[WA_CC_SPLIT_NUM_TABLES];
static struct { int max_samples; cc_cmf cmf[WA_CC_MAX_PULSES_PER_SF]; } g_runlen[WA_CC_NUM_RUNLEN];
static int g_ready;

static uint16_t *alloc_cmf(int n, cc_cmf *out)
{
    out->data = (uint16_t *)malloc((size_t)(n + 1) * sizeof(uint16_t));
    out->len = n + 1;
    return out->data;
}

static void build_split(void)
{
    int32_t pdf[WA_CC_SPLIT_NUM_TABLES + 2];
    for (int numPulses = 1; numPulses <= WA_CC_SPLIT_NUM_TABLES; numPulses++) {
        int minSplit = numPulses - WA_CC_MAX_PULSES_PER_SF * 2;
        if (minSplit < 0) minSplit = 0;
        int maxSplit = numPulses - minSplit;
        int cnt = maxSplit - minSplit + 1;
        for (int i = 0; i < cnt; i++) pdf[i] = cc_prob_split_fast(minSplit + i, numPulses);
        uint16_t *dst = alloc_cmf(cnt, &g_split[numPulses - 1]);
        pdf_to_cmf(pdf, cnt, dst);
    }
}

static void build_runlen(void)
{
    for (int oi = 0; oi < WA_CC_NUM_RUNLEN; oi++) {
        int ms = (oi + 1) * WA_CC_RUNLENGTH_STEP;
        g_runlen[oi].max_samples = ms;
        int32_t *pdf = (int32_t *)malloc((size_t)ms * sizeof(int32_t));
        for (int nump = 1; nump <= WA_CC_MAX_PULSES_PER_SF; nump++) {
            int64_t plonger = CC_ONE_Q31;
            for (int nums = 1; nums <= ms; nums++) {
                int64_t tmp = CC_ONE_Q31 - (CC_ONE_Q31 / (int64_t)(ms - nums + 1));
                int64_t p1 = tmp;
                for (int r = 0; r < nump - 1; r++) p1 = (p1 * tmp) >> 31;
                p1 = CC_ONE_Q31 - p1;
                if (p1 > 2147376274) p1 = 2147376274;
                int32_t logOutQ7;
                if (nump > ms) logOutQ7 = cc_lin2log((nump << 10) / ms) - 10 * 128;
                else logOutQ7 = -(cc_lin2log((ms << 10) / nump) - 10 * 128);
                const int32_t sigmBias = 146, scaleMax = 36000, scaleMin = 26000;
                int32_t scaleFac = scaleMax - (((scaleMax - scaleMin) * cc_sigm_q15((logOutQ7 >> 2) + sigmBias)) >> 15);
                p1 = CC_ONE_Q31 - (int64_t)cc_log2lin(((scaleFac * (cc_lin2log((int32_t)(CC_ONE_Q31 - p1)) - 31 * 128)) >> 15) + 31 * 128);
                if (p1 > 2147376274) p1 = 2147376274;
                pdf[nums - 1] = (int32_t)((plonger * p1) >> 31);
                plonger = (plonger * (CC_ONE_Q31 - p1)) >> 31;
            }
            uint16_t *dst = alloc_cmf(ms, &g_runlen[oi].cmf[nump - 1]);
            pdf_to_cmf(pdf, ms, dst);
        }
        free(pdf);
    }
}

int wa_cc_init(void)
{
    if (g_ready) return 0;
    build_split();
    build_runlen();
    g_ready = 1;
    return 0;
}

const uint16_t *wa_cc_split_cmf(int total, size_t *len)
{
    int i = total - 1;
    if (i < 0 || i >= WA_CC_SPLIT_NUM_TABLES) { *len = 0; return NULL; }
    *len = (size_t)g_split[i].len;
    return g_split[i].data;
}

const uint16_t *wa_cc_runlen_cmf(int oct, int c, size_t *len)
{
    if (oct < 1 || oct > WA_CC_NUM_RUNLEN || c < 1 || c > WA_CC_MAX_PULSES_PER_SF) { *len = 0; return NULL; }
    cc_cmf *m = &g_runlen[oct - 1].cmf[c - 1];
    *len = (size_t)m->len;
    return m->data;
}

int wa_cc_runlen_max(int oct) { return (oct >= 1 && oct <= WA_CC_NUM_RUNLEN) ? g_runlen[oct - 1].max_samples : 0; }

int wa_mlow_cc_selftest(void)
{
    if (wa_cc_init() != 0) return -1;
    // sanity: CMFs are non-decreasing and start at 0
    size_t n;
    const uint16_t *s = wa_cc_split_cmf(10, &n);
    if (!s || n < 2 || s[0] != 0) return -1;
    for (size_t i = 1; i < n; i++) if (s[i] < s[i - 1]) return -1;
    const uint16_t *r = wa_cc_runlen_cmf(5, 3, &n);
    if (!r || n < 2 || r[0] != 0) return -1;
    for (size_t i = 1; i < n; i++) if (r[i] < r[i - 1]) return -1;
    return 0;
}
