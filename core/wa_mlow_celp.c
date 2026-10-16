// MLow CELP decoder excitation. Faithful float32 port of meowcaller celpdec.go (whatsapp-rust
// smpl_celpdec.rs, MIT): FCB pulses x gain + voiced ACB/LTP (pitch) synthesis. The excitation is captured
// before the noise/LPC-synthesis stages (which do not feed back into it), so it is KAT-verified on its own.
#include "wa_mlow_celp.h"
#include "wa_mlow.h"
#include "wa_mlow_rc.h"
#include "wa_mlow_cc.h"
#include "wa_mlow_noise.h"
#include "wa_util.h"

#include <math.h>
#include <string.h>

#define INTF WA_CELP_INTF_LEN
#define LAG_SUBFR 40
#define LTP_DELAY WA_CELP_LTP_INTERPOL_DELAY
#define MAX_PITCH_LAG WA_CELP_MAX_PITCH_LAG
#define ACBG_M 2
#define ACBG_N 16

static const float PITCH_SHARP_COEF = 0.9881f;
static const float DEC_ACB_HIGH_BOOST[2] = { 0.35f, 0.18f };

// 16-tap symmetric LTP interpolation kernel.
static const float INTERP_KERNEL[2 * LTP_DELAY] = {
    -6.3925986e-6f, 0.00011064114f, -0.0009153038f, 0.00484772f, -0.018698348f, 0.05759091f, -0.15997477f, 0.6170455f,
    0.61704546f, -0.15997475f, 0.057590906f, -0.018698348f, 0.00484772f, -0.0009153038f, 0.000110641144f, -6.392598e-6f,
};
// ACB-gain codebooks (Q14), [acbgN*acbgM]. cb_acbgains_{hr,lr}_q14 from smpl_celp.
static const int16_t CB_ACBG_HR_Q14[ACBG_N * ACBG_M] = {
    16039, 91, 0, 0, 4310, 4930, -1431, 2862, 2893, 0, 8009, 4075, 2754, 4223, 8367, 354,
    4640, 1254, -176, 2734, -1222, 5017, -476, 1506, 11351, 567, 1243, 0, 10601, 22, 14088, 108,
};
static const int16_t CB_ACBG_LR_Q14[ACBG_N * ACBG_M] = {
    2812, 2484, 0, 0, -362, 2465, -337, 703, 3033, 1474, 13536, 220, -2630, 9226, 6032, 3499,
    -220, 441, 7661, 4243, 11521, 0, 1430, 779, 4495, 2724, 15535, 343, -779, 1559, 480, 481,
};

// FCB gain tables (built once): uv[0..90] = 10^(0.05*(ix*1-90)); v[0..33] = 10^(0.05*(ix*3-100)).
static float g_fcb_uv[91];
static float g_fcb_v[34];
static int g_fcb_ready;
static void fcb_gains(void)
{
    if (g_fcb_ready) return;
    for (int ix = 0; ix <= 90; ix++) g_fcb_uv[ix] = (float)pow(10.0, (double)(0.05f * ((float)ix * 1.0f - 90.0f)));
    for (int ix = 0; ix < 34; ix++) g_fcb_v[ix] = (float)pow(10.0, (double)(0.05f * ((float)ix * 3.0f - 100.0f)));
    g_fcb_ready = 1;
}

static float celp_dot(const float *a, const float *b, int l) { float r = 0; for (int i = 0; i < l; i++) r += a[i] * b[i]; return r; }
static float absf(float x) { return x < 0 ? -x : x; }

static void acb_dequant(int low_rate, int32_t acb_idx, float acb_g[ACBG_M])
{
    const int16_t *cb = low_rate ? CB_ACBG_LR_Q14 : CB_ACBG_HR_Q14;
    const float sc = 1.0f / (float)((int32_t)1 << 14);
    for (int m = 0; m < ACBG_M; m++) acb_g[m] = (float)cb[(int)acb_idx * ACBG_M + m] * sc;
}

static void acb_synthesize(int fcb_subfrlen, const float *acb_basis, const float acb_g_in[ACBG_M], float high_boost, float *acb)
{
    float acb_g[ACBG_M] = { acb_g_in[0], acb_g_in[1] };
    if (high_boost != 0.0f) {
        float f0 = acb_g[0] + 2.0f * acb_g[1];
        float f1 = acb_g[0] - acb_g[1];
        float abs_f2new = f1 < 0 ? -f1 : f1;
        abs_f2new += high_boost;
        float abs_f0 = f0 < 0 ? -f0 : f0;
        if (abs_f0 < abs_f2new) abs_f2new = abs_f0;
        f1 = f1 * (abs_f2new / (absf(f1) + 1e-12f));
        acb_g[0] = (f0 + 2.0f * f1) / 3.0f;
        acb_g[1] = (f0 - f1) / 3.0f;
    }
    for (int i = 0; i < fcb_subfrlen; i++) acb[i] = acb_g[0] * acb_basis[i];
    for (int i = 0; i < fcb_subfrlen; i++) acb[i] += acb_g[1] * acb_basis[fcb_subfrlen + i];
}

static void pitch_sharp(float *x, int lag, int l)
{
    for (int i = lag; i < l; i++) x[i] += x[i - lag] * PITCH_SHARP_COEF;
}

// build the ACB basis from the excitation history; mutates state[] forward.
static void syn_ltp_basis(const float *lags, int n_lags, float *state, int state_len, float *acb_basis)
{
    int p = state_len - n_lags * LAG_SUBFR;
    for (int subfr = 0; subfr < n_lags; subfr++) {
        int i_lag = (int)floorf(lags[subfr]);
        if ((float)i_lag == lags[subfr]) {
            int il = i_lag;
            for (int i = 0; i < LAG_SUBFR; i++) state[p + i] = state[p + i - il];
            for (int i = 0; i < LAG_SUBFR; i++) acb_basis[subfr * LAG_SUBFR + i] = state[p + i];
            for (int i = 0; i < LAG_SUBFR; i++) acb_basis[(n_lags + subfr) * LAG_SUBFR + i] = state[p + i - il - 1] + state[p + i - il + 1];
        } else {
            int il = i_lag;
            int base_first = p + (-1 - il - LTP_DELAY);
            float first = celp_dot(&state[base_first], INTERP_KERNEL, 2 * LTP_DELAY);
            int src_base = p + (-il - LTP_DELAY);
            for (int nn = 0; nn < LAG_SUBFR; nn++) {
                float ret = 0.0f;
                for (int i = 0; i < 8; i++) {
                    float s0 = state[src_base + nn + i];
                    float s1 = state[src_base + nn + 15 - i];
                    ret += (s0 + s1) * INTERP_KERNEL[i];
                }
                state[p + nn] = ret;
            }
            int base_last = p + (LAG_SUBFR - il - LTP_DELAY);
            float last = celp_dot(&state[base_last], INTERP_KERNEL, 2 * LTP_DELAY);
            for (int i = 0; i < LAG_SUBFR; i++) acb_basis[subfr * LAG_SUBFR + i] = state[p + i];
            int b1 = (n_lags + subfr) * LAG_SUBFR;
            acb_basis[b1] = first + state[p + 1];
            for (int i = 0; i < LAG_SUBFR - 2; i++) acb_basis[b1 + 1 + i] = state[p + i] + state[p + i + 2];
            int i_last = LAG_SUBFR - 1;
            acb_basis[b1 + i_last] = state[p + i_last - 1] + last;
        }
        p += LAG_SUBFR;
    }
}

// add the ACB (LTP) contribution into lpc_res (voiced), then push the subframe into the ACB state.
static void celp_decode(float *acb_state, int acb_state_len, int voiced, int32_t acb_gain_idx, const float *lags,
                        int num_lags, int subfrlen, int low_rate, float norm_br, float *lpc_res)
{
    if (voiced) {
        float high_boost = DEC_ACB_HIGH_BOOST[0] + (DEC_ACB_HIGH_BOOST[1] - DEC_ACB_HIGH_BOOST[0]) * norm_br;
        int i_lag = (int)lags[num_lags - 1];
        if (low_rate) pitch_sharp(lpc_res, i_lag, subfrlen);
        float acb_basis[160 * ACBG_M];
        float acb[160];
        syn_ltp_basis(lags, num_lags, acb_state, acb_state_len, acb_basis);
        float acb_gain[ACBG_M];
        acb_dequant(low_rate, acb_gain_idx, acb_gain);
        acb_synthesize(subfrlen, acb_basis, acb_gain, high_boost, acb);
        for (int i = 0; i < subfrlen; i++) lpc_res[i] += acb[i];
    }
    memmove(&acb_state[0], &acb_state[subfrlen], (size_t)(acb_state_len - 2 * subfrlen) * sizeof(float));
    memcpy(&acb_state[acb_state_len - 2 * subfrlen], lpc_res, (size_t)subfrlen * sizeof(float));
}

void wa_mlow_celp_init(wa_mlow_celp_state *st) { memset(st, 0, sizeof(*st)); }

void wa_mlow_celp_exc_pre(wa_mlow_celp_state *st, const int32_t *pulses, const wa_mlow_celp_params *params,
                          int low_rate, float *exc)
{
    fcb_gains();
    int num_subframes = low_rate ? 2 : 4;
    int subframe_len = low_rate ? (INTF / 2) : WA_CELP_SUBFR_LEN;
    float norm_br = wa_mlow_norm_bitrate(params->total_pulses, 320);

    float lpc_res[INTF];
    memset(lpc_res, 0, sizeof(lpc_res));
    const float *gain_tab = params->voiced ? g_fcb_v : g_fcb_uv;
    for (int pos = 0; pos < INTF; pos++) {
        if (pulses[pos] != 0) {
            int sf = pos / subframe_len;
            lpc_res[pos] = (float)pulses[pos] * gain_tab[params->fcbg_idx[sf]];
        }
    }

    int lags_per_subfr = subframe_len / LAG_SUBFR;
    int acb_state_len = subframe_len + 2 * MAX_PITCH_LAG + LTP_DELAY;
    for (int sf = 0; sf < num_subframes; sf++) {
        int base = sf * subframe_len;
        const float *sf_lags = &params->block_lags[sf * lags_per_subfr];
        celp_decode(st->acb_state, acb_state_len, params->voiced, params->acbg_idx[sf], sf_lags, lags_per_subfr,
                    subframe_len, low_rate, norm_br, &lpc_res[base]);
        memcpy(&exc[base], &lpc_res[base], (size_t)subframe_len * sizeof(float));
    }
}

// --- KAT (exc_pre_lags.json + inbound_capture_frames.json): replay every frame in order (ACB + LSF state +
// pitch predictor carry), compare the captured exc_pre where a C record exists. Tol 2e-5. ---
static float bf(uint32_t u) { union { uint32_t u; float f; } x; x.u = u; return x.f; }

extern const int wa_mlow_excpre_nframes;
extern const int wa_mlow_excpre_frameoff[];
extern const int wa_mlow_excpre_framelen[];
extern const uint8_t wa_mlow_excpre_frames[];
extern const int wa_mlow_excpre_nrecs;
extern const int wa_mlow_excpre_rec_packet[];
extern const int wa_mlow_excpre_rec_frame[];
extern const int wa_mlow_excpre_rec_sf[];
extern const int wa_mlow_excpre_rec_nzoff[];
extern const int wa_mlow_excpre_rec_nzcount[];
extern const int wa_mlow_excpre_nzpos[];
extern const uint32_t wa_mlow_excpre_nzval[];

int wa_mlow_celp_selftest(void)
{
    if (wa_cc_init() != 0 || wa_cc_seed_init() != 0 || wa_mlow_pitch_init() != 0) { wa_log("mlow celp: init failed"); return -1; }

    // Build a (packet,frame,sf) -> record index map by linear scan (records are small).
    wa_mlow_celp_state celp;
    wa_mlow_celp_init(&celp);
    wa_mlow_lsf_state lstate;
    memset(&lstate, 0, sizeof lstate);
    float worst = 0.0f;
    int checked = 0;

    for (int packet = 0; packet < wa_mlow_excpre_nframes; packet++) {
        const uint8_t *frame = &wa_mlow_excpre_frames[wa_mlow_excpre_frameoff[packet]];
        int flen = wa_mlow_excpre_framelen[packet];
        if (flen == 0) continue;
        wa_mlow_toc toc;
        wa_mlow_parse_toc(frame[0], &toc);
        if (toc.std_opus || toc.sid || !toc.active) continue;
        int config = (frame[0] >> 2) & 1;
        int low_rate = config;
        wa_rc dec;
        wa_rc_init(&dec, frame + 1, (size_t)(flen - 1));
        for (int f = 0; f < 3; f++) {
            wa_mlow_lsf_indices lsf;
            wa_mlow_decode_lsf(&dec, config, f, 1, &lstate, &lsf);
            wa_mlow_pulse_result pr;
            wa_mlow_decode_pulses(&dec, 320, 4, 1, config, lsf.stage1, &pr);
            int voiced = (lsf.stage1 == 1);
            wa_mlow_celp_params params;
            memset(&params, 0, sizeof params);
            params.voiced = voiced;
            int32_t total = 0;
            for (int k = 0; k < 4; k++) total += pr.subfr[k];
            params.total_pulses = total;
            if (voiced) {
                wa_mlow_pitch_result pit;
                wa_mlow_decode_pitch(&dec, &lstate, 4, config, pr.subfr, &pit);
                for (int b = 0; b < 8; b++) {
                    float v = (float)pit.block_lags[b] * 0.5f + 32.0f;
                    if (v > 320.0f) v = 320.0f;
                    params.block_lags[b] = v;
                }
                for (int sf = 0; sf < 4; sf++) {
                    params.acbg_idx[sf] = pit.gain_idx[sf];
                    if (pit.filt_idx[sf] > 0) params.fcbg_idx[sf] = pit.filt_idx[sf];
                }
            } else {
                wa_mlow_gain_result g;
                wa_mlow_decode_gains(&dec, 4, pr.subfr, &g);
                for (int sf = 0; sf < 4; sf++) params.fcbg_idx[sf] = g.nrg_res[sf];
            }
            float exc[INTF];
            wa_mlow_celp_exc_pre(&celp, pr.pulses, &params, low_rate, exc);

            // compare against any matching C record
            for (int r = 0; r < wa_mlow_excpre_nrecs; r++) {
                if (wa_mlow_excpre_rec_packet[r] != packet || wa_mlow_excpre_rec_frame[r] != f) continue;
                int sf = wa_mlow_excpre_rec_sf[r];
                float cexc[WA_CELP_SUBFR_LEN];
                memset(cexc, 0, sizeof cexc);
                int off = wa_mlow_excpre_rec_nzoff[r], cnt = wa_mlow_excpre_rec_nzcount[r];
                for (int j = 0; j < cnt; j++) cexc[wa_mlow_excpre_nzpos[off + j]] = bf(wa_mlow_excpre_nzval[off + j]);
                int base = sf * WA_CELP_SUBFR_LEN;
                for (int i = 0; i < WA_CELP_SUBFR_LEN; i++) {
                    float d = absf(exc[base + i] - cexc[i]);
                    if (d > worst) worst = d;
                    if (d > 2e-5f) { wa_log("mlow exc_pre pkt=%d f=%d sf=%d i=%d: %.7g vs %.7g (d=%.2e)", packet, f, sf, i, exc[base + i], cexc[i], d); return -1; }
                }
                checked++;
            }
        }
    }
    if (checked == 0) { wa_log("mlow exc_pre: no records checked"); return -1; }
    return 0;
}
