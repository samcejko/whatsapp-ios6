// MLow low-band synthesis: NLSF reconstruction + NLSF->LPC. Faithful float32 port of meowcaller synth.go
// (SmplReconstructNLSF, SmplNLSF2A) + the DECODER subset of lsf_seed.go buildLsfFromSeed (Valtables/Centroids/
// Matrices/MinSpacing/Grid16*). whatsapp-rust smpl_synth.rs + smpl_lsf_seed.rs (MIT).
#include "wa_mlow_synth.h"
#include "wa_util.h"

#include <math.h>
#include <string.h>

#define ORD 16
#define NCENT 16          // LSFCBCentroids
#define ST2_LEN 9593

static const float PI_F32 = 3.1415927410125f;   // == float32(math.Pi)

// per-voiced scale/min constants (lsf_seed.go)
static const float CB_MIN[2]        = { -0.5873778f, -0.24721986f };
static const float CB_SCALE[2]      = { 1.3145164e-5f, 7.226229e-6f };
static const float ROT_MIN[2]       = { -0.9124832f, -0.8455929f };
static const float ROT_SCALE[2]     = { 0.006554049f, 0.0069253775f };
static const float ROTC_MIN[2]      = { -0.67291605f, -0.8248211f };
static const float ROTC_SCALE[2]    = { 0.0052386564f, 0.0064186584f };
static const float ST2_QLVLS_MIN    = -0.45f;
static const float ST2_QLVLS_SCALE  = 0.0034478905f;
static const float QSTEP_COND_MULT  = 0.9f;
static const float NLSF_WMAX        = 999.9999f;
static const float NLSF_WEPS        = 0.0009999999f;
static const float STAB_EPS         = 9.5367431640625e-07f;
#define STAB_MAX_LOOP 1000

// --- embedded inflated lsf_seed protobuf ROM ---
extern const uint8_t wa_mlow_lsf_seed[];
extern const int wa_mlow_lsf_seed_len;

// Decoder subset of the synth tables.
static float g_cbhalf[2][NCENT][ORD];          // Centroids
static float g_we[2][NCENT][ORD][ORD];         // Matrices (we[c][i][j])
static float g_rotcond[2][2][ORD * ORD];       // Grid16Matrices (flattened 256)
static float g_qlvls[ST2_LEN];                 // flat Valtables storage
static int g_qoff[2][2][17][ORD];
static int g_qcnt[2][2][17][ORD];
static float g_mean[2][ORD];
static float g_min_dist[2][17];
static float g_reg_cond[2];
static float g_qstep[2][2];
static int g_ready;

static float maxf(float a, float b) { return a > b ? a : b; }

// --- protobuf (all seed fields are wiretype 2 byte blobs) ---
typedef struct { const uint8_t *bytes; size_t len; int present; } pf;
static uint64_t prv(const uint8_t *b, size_t n, size_t *i)
{
    uint64_t v = 0; int s = 0;
    while (*i < n) { uint8_t c = b[(*i)++]; v |= (uint64_t)(c & 0x7f) << s; if (!(c & 0x80)) break; s += 7; }
    return v;
}
static void parse_seed(pf *f /* [17] */)
{
    memset(f, 0, sizeof(pf) * 17);
    const uint8_t *b = wa_mlow_lsf_seed;
    size_t n = (size_t)wa_mlow_lsf_seed_len, i = 0;
    while (i < n) {
        uint64_t key = prv(b, n, &i);
        int field = (int)(key >> 3), wire = (int)(key & 7);
        if (wire != 2) break;
        size_t ln = (size_t)prv(b, n, &i);
        if (i + ln > n) break;
        if (field >= 1 && field <= 16) { f[field].bytes = b + i; f[field].len = ln; f[field].present = 1; }
        i += ln;
    }
}

// decode packed varint u32 stream into out (up to max), return count.
static int dec_varints(const uint8_t *b, size_t n, uint32_t *out, int max)
{
    size_t i = 0; int c = 0;
    while (i < n && c < max) {
        uint64_t v = 0; int s = 0;
        while (i < n) { uint8_t ch = b[i++]; v |= (uint64_t)(ch & 0x7f) << s; if (!(ch & 0x80)) break; s += 7; }
        out[c++] = (uint32_t)v;
    }
    return c;
}
static float f32le(const uint8_t *b) { union { uint32_t u; float f; } x; x.u = (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24)); return x.f; }

// Laroia inverse-gap weights with the gap floored at 1e-3 (lsf_seed.go lsfSeedLaroia).
static void seed_laroia(const float lsf[ORD], float w[ORD])
{
    const float md = 1e-3f;
    float inv[ORD + 1];
    inv[0] = 1.0f / maxf(lsf[0], md);
    for (int i = 1; i < ORD; i++) inv[i] = 1.0f / maxf(lsf[i] - lsf[i - 1], md);
    inv[ORD] = 1.0f / maxf(PI_F32 - lsf[ORD - 1], md);
    for (int i = 0; i < ORD; i++) w[i] = inv[i] + inv[i + 1];
}

int wa_mlow_synth_init(void)
{
    if (g_ready) return 0;
    pf f[17];
    parse_seed(f);
    if (!f[1].present || !f[2].present || !f[3].present || !f[5].present || !f[6].present ||
        !f[7].present || !f[13].present || !f[14].present || !f[15].present || !f[16].present)
        return -1;

    // cb16 [2][16][16] (varint u32 -> u16)
    static uint32_t cbv[2 * NCENT * ORD];
    int ncb = dec_varints(f[7].bytes, f[7].len, cbv, 2 * NCENT * ORD);
    if (ncb < 2 * NCENT * ORD) return -1;

    // floats: mean [2][16], min_dist [2][17], reg_cond [2], qstep [2][2]
    for (int v = 0; v < 2; v++) for (int i = 0; i < ORD; i++) g_mean[v][i] = f32le(f[13].bytes + 4 * (v * ORD + i));
    for (int v = 0; v < 2; v++) for (int i = 0; i < 17; i++) g_min_dist[v][i] = f32le(f[14].bytes + 4 * (v * 17 + i));
    g_reg_cond[0] = f32le(f[15].bytes); g_reg_cond[1] = f32le(f[15].bytes + 4);
    for (int v = 0; v < 2; v++) for (int i = 0; i < 2; i++) g_qstep[v][i] = f32le(f[16].bytes + 4 * (v * 2 + i));

    // per-voiced centroid tables
    for (int v = 0; v < 2; v++) {
        for (int c = 0; c < NCENT; c++) {
            float lsf_cb[ORD];
            for (int i = 0; i < ORD; i++) {
                float prod = (float)cbv[(v * NCENT + c) * ORD + i] * CB_SCALE[v];
                lsf_cb[i] = CB_MIN[v] + prod + g_mean[v][i];
                g_cbhalf[v][c][i] = lsf_cb[i] * 0.5f;
            }
            // rot = unpack8(rot8[v][c]); we[i][j] = rot[i][j] / sqrt(laroia(lsf_cb))[j]
            const uint8_t *rot8 = f[1].bytes + ((size_t)(v * NCENT + c)) * ORD * ORD;
            float lsfw[ORD];
            seed_laroia(lsf_cb, lsfw);
            for (int i = 0; i < ORD; i++) lsfw[i] = (float)sqrt((double)lsfw[i]);
            float lsfw_inv[ORD];
            for (int i = 0; i < ORD; i++) lsfw_inv[i] = 1.0f / lsfw[i];
            for (int i = 0; i < ORD; i++) {
                for (int j = 0; j < ORD; j++) {
                    float rot = ROT_MIN[v] + (float)((float)rot8[i * ORD + j] * ROT_SCALE[v]);
                    g_we[v][c][i][j] = rot * lsfw_inv[j];
                }
            }
        }
        // rotcond[v][lr] = unpack8(rot_cond_8[v][lr]) flattened
        for (int lr = 0; lr < 2; lr++) {
            const uint8_t *rc8 = f[2].bytes + ((size_t)(v * 2 + lr)) * ORD * ORD;
            for (int k = 0; k < ORD * ORD; k++) g_rotcond[v][lr][k] = ROTC_MIN[v] + (float)((float)rc8[k] * ROTC_SCALE[v]);
        }
    }

    // stage-2 qlvls: walk st2Qlvls8 per [v][lr][c=0..16][i]; count = maxQi-minQi+1
    const int8_t *minqi = (const int8_t *)f[5].bytes;
    const int8_t *maxqi = (const int8_t *)f[6].bytes;
    const uint8_t *q8 = f[3].bytes;
    int qptr = 0, q8ptr = 0, qip = 0;   // qip walks minqi/maxqi [2][2][17][16]
    for (int v = 0; v < 2; v++) {
        for (int lr = 0; lr < 2; lr++) {
            for (int c = 0; c < 17; c++) {
                float qstep = g_qstep[v][lr];
                if (c == NCENT) qstep *= QSTEP_COND_MULT;
                for (int i = 0; i < ORD; i++) {
                    int32_t mn = minqi[qip], mx = maxqi[qip];
                    qip++;
                    int nq = (int)(mx - mn + 1);
                    g_qoff[v][lr][c][i] = qptr;
                    g_qcnt[v][lr][c][i] = nq;
                    for (int lvl = 0; lvl < nq; lvl++) {
                        if (qptr >= ST2_LEN || q8ptr >= (int)f[3].len) return -1;
                        float prod = ST2_QLVLS_SCALE * (float)q8[q8ptr];
                        g_qlvls[qptr] = (ST2_QLVLS_MIN + prod + (float)lvl + (float)mn) * qstep;
                        qptr++; q8ptr++;
                    }
                }
            }
        }
    }
    if (qptr != ST2_LEN || q8ptr != ST2_LEN) return -1;

    g_ready = 1;
    return 0;
}

// synth Laroia weights (clamped), smplNLSFLaroiaWeights.
static void nlsf_laroia(const float *nlsf, float *out)
{
    float inv[ORD + 1];
    float g0 = nlsf[0];
    inv[0] = (g0 > NLSF_WEPS) ? 1.0f / g0 : NLSF_WMAX;
    float prev = nlsf[0];
    for (int k = 1; k < ORD; k++) {
        float gap = nlsf[k] - prev;
        inv[k] = (gap > NLSF_WEPS) ? 1.0f / gap : NLSF_WMAX;
        prev = nlsf[k];
    }
    float gl = PI_F32 - nlsf[ORD - 1];
    inv[ORD] = (gl > NLSF_WEPS) ? 1.0f / gl : NLSF_WMAX;
    for (int k = 0; k < ORD; k++) out[k] = inv[k] + inv[k + 1];
}

// out[r] = sum_c mat[c*16 + r] * vec[c]  (column-major decorrelation).
static void nlsf_decorr(const float *mat, const float *vec, float *out)
{
    float scr[ORD];
    float v0 = vec[0];
    for (int r = 0; r < ORD; r++) scr[r] = v0 * mat[r];
    for (int c = 1; c < ORD; c++) {
        float v = vec[c];
        int base = c * ORD;
        for (int r = 0; r < ORD; r++) scr[r] += mat[base + r] * v;
    }
    memcpy(out, scr, sizeof(scr));
}

// enforce minimum spacing + ordering in the margin domain (silk_NLSF_stabilize).
static void stabilize_nlsf(float *nlsf, const float *min_spacing)
{
    float marg[ORD + 1];
    marg[0] = nlsf[0] - min_spacing[0];
    for (int i = 1; i < ORD; i++) marg[i] = nlsf[i] - nlsf[i - 1] - min_spacing[i];
    marg[ORD] = PI_F32 - nlsf[ORD - 1] - min_spacing[ORD];

    float mn = marg[0]; int sel = 0;
    for (int i = 1; i < ORD + 1; i++) if (marg[i] < mn) { mn = marg[i]; sel = i; }
    int loopn = 0;
    while (mn < 0.0f) {
        float d = (float)loopn * STAB_EPS - mn;
        if (sel == 0) { marg[0] += d; marg[1] -= d; }
        else if (sel == ORD) { marg[ORD] += d; marg[ORD - 1] -= d; }
        else { marg[sel] += d; float half = d * 0.5f; marg[sel - 1] -= half; marg[sel + 1] -= half; }
        mn = marg[0]; sel = 0;
        for (int i = 1; i < ORD + 1; i++) if (marg[i] < mn) { mn = marg[i]; sel = i; }
        if (mn < 0.0f) { loopn++; if (loopn == STAB_MAX_LOOP) break; }
    }
    nlsf[0] = min_spacing[0] + marg[0];
    float run = nlsf[0];
    for (int i = 1; i < ORD; i++) { run = run + marg[i] + min_spacing[i]; nlsf[i] = run; }
}

void wa_mlow_reconstruct_nlsf(int stage1, int config, int grid, const int32_t stage2[16],
                              const float *prev_nlsf, float *out)
{
    if (stage1 < 0) stage1 = 0; if (stage1 > 1) stage1 = 1;
    if (config < 0) config = 0; if (config > 1) config = 1;
    if (grid < 0) grid = 0; if (grid > 16) grid = 16;

    float resid[ORD];
    for (int k = 0; k < ORD; k++) {
        int sym = (int)stage2[k];
        int off = g_qoff[stage1][config][grid][k], cnt = g_qcnt[stage1][config][grid][k];
        resid[k] = (sym >= 0 && sym < cnt) ? g_qlvls[off + sym] : 0.0f;
    }

    if (grid == 16) {
        float base[ORD];
        const float *base_tbl = g_mean[1 - stage1];   // Grid16W[v] = mean[1-v]
        float alpha = g_reg_cond[stage1];              // Grid16Alpha[v] = reg_cond[v]
        for (int k = 0; k < ORD; k++) {
            float pv = prev_nlsf ? prev_nlsf[k] : 0.0f;
            base[k] = pv + alpha * (base_tbl[k] - pv);
        }
        float w[ORD];
        nlsf_laroia(base, w);
        for (int i = 0; i < ORD; i++) w[i] = (float)sqrt((double)w[i]);
        float decorr[ORD];
        nlsf_decorr(g_rotcond[stage1][config], resid, decorr);
        for (int k = 0; k < ORD; k++) out[k] = base[k] + decorr[k] / w[k];
        stabilize_nlsf(out, g_min_dist[1 - stage1]);   // MinSpacing[v] = min_dist[1-v]
        return;
    }

    const float *cent = g_cbhalf[stage1][grid];
    for (int r = 0; r < ORD; r++) {
        float acc = 2.0f * cent[r];
        for (int c = 0; c < ORD; c++) acc += g_we[stage1][grid][c][r] * resid[c];
        out[r] = acc;
    }
    stabilize_nlsf(out, g_min_dist[1 - stage1]);
}

static void nlsf_poly(double *out, const double *cosv, int half, int parity)
{
    out[0] = 1.0;
    out[1] = -2.0 * cosv[parity];
    for (int k = 1; k < half; k++) {
        double c = -2.0 * cosv[2 * k + parity];
        out[k + 1] = 2.0 * out[k - 1] + c * out[k];
        for (int nn = k; nn > 1; nn--) out[nn] += out[nn - 2] + c * out[nn - 1];
        out[1] += c;
    }
}

void wa_mlow_nlsf2a(const float *nlsf, float *a)
{
    int order = ORD, half = order / 2;
    double cosv[ORD], p[ORD / 2 + 1], q[ORD / 2 + 1];
    for (int i = 0; i < order; i++) cosv[i] = cos((double)nlsf[i]);
    nlsf_poly(p, cosv, half, 0);
    nlsf_poly(q, cosv, half, 1);
    a[0] = 1.0f;
    for (int k = 0; k < half; k++) {
        double pt = p[k + 1] + p[k];
        double qt = q[k + 1] - q[k];
        a[k + 1] = (float)(0.5 * (pt + qt));
        a[order - k] = (float)(0.5 * (pt - qt));
    }
}

// --- sanity self-test: tables build, NLSF2A yields a monic stable-ish polynomial, reconstruct is ordered ---
int wa_mlow_synth_selftest(void)
{
    if (wa_mlow_synth_init() != 0) { wa_log("mlow synth: init failed"); return -1; }
    // NLSF2A of a monotone-increasing NLSF -> a[0]==1 and finite coefficients.
    float nlsf[ORD], a[ORD + 1];
    for (int i = 0; i < ORD; i++) nlsf[i] = (float)(i + 1) * (PI_F32 / (float)(ORD + 1));
    wa_mlow_nlsf2a(nlsf, a);
    if (a[0] != 1.0f) return -1;
    for (int i = 0; i <= ORD; i++) if (!(a[i] == a[i])) { wa_log("mlow synth: nlsf2a NaN at %d", i); return -1; }
    // reconstruct with zero stage-2 symbols -> strictly increasing, within (0, pi).
    int32_t st2[16]; memset(st2, 0, sizeof st2);
    float out[ORD];
    wa_mlow_reconstruct_nlsf(1, 0, 0, st2, NULL, out);
    if (!(out[0] > 0.0f) || !(out[ORD - 1] < PI_F32)) { wa_log("mlow synth: reconstruct out of (0,pi)"); return -1; }
    for (int i = 1; i < ORD; i++) if (!(out[i] > out[i - 1])) { wa_log("mlow synth: reconstruct not increasing at %d", i); return -1; }
    return 0;
}
