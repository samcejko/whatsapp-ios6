// MLow CELP decoder-side noise generator. Faithful float32 port of meowcaller noise.go
// (whatsapp-rust smpl_gennoise.rs, MIT). All arithmetic is float32 except where the reference computes a
// transcendental in double and casts back - replicated here with (float)<libm double call>.
#include "wa_mlow_noise.h"
#include "wa_util.h"

#include <math.h>
#include <string.h>

#define NSF  WA_NOISE_MAX_SF_LEN
#define NCORR WA_NOISE_CORR_ORDER
#define NDCT WA_NOISE_DCT_ORDER
#define FS_KHZ 16
static const float PI_NOISE = 3.1415926535897f;

static const float DEC_V_NOISE_GAIN = 0.35f;
static const float DEC_UV_NOISE_GAIN = 0.8f;
static const float DEC_UV_FCORNER_HZ = 800.0f;
static const float ENV_SMTH_V = 0.95f;
static const float ENV_SMTH_UV = 0.995f;
static const float ENV_SMTH_UVV = 0.99f;
static const float RESNRG_BIAS = 3.1622776e-9f;
static const float COEF_MA_V[3] = { 0.25f, -0.496f, 0.25f };

static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }

int32_t wa_mlow_smpl_rand(int32_t seed)
{
    return (int32_t)(907633515u + (uint32_t)seed * 196314165u);
}

static float smpl_sigmoid(float x)
{
    if (x > 80.0f) return 1.0f;
    if (x < -80.0f) return 0.0f;
    return 1.0f / (1.0f + (float)exp((double)(-x)));
}

static float smpl_nrg(const float *x, int n)
{
    float nrg = 0.0f;
    for (int i = 0; i < n; i++) nrg += x[i] * x[i];
    return nrg;
}

static float smpl_sum(const float *x, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; i++) s += x[i];
    return s;
}

static float smpl_maximum(const float *x, int n)
{
    float m = x[0];
    for (int i = 1; i < n; i++) if (x[i] > m) m = x[i];
    return m;
}

// 4-at-a-time bit-rotated white pulses scaled by 8.1e-10.
static void smpl_gen_rand_pulses(float *noise, int l, int32_t *seed)
{
    const float sc = 8.1e-10f;
    int i = 0;
    for (; i + 3 < l; i += 4) {
        *seed = wa_mlow_smpl_rand(*seed);
        uint32_t s = (uint32_t)*seed;
        noise[i] = sc * (float)*seed;
        noise[i + 1] = sc * (float)(int32_t)(s << 8);
        noise[i + 2] = sc * (float)(int32_t)(s << 16);
        noise[i + 3] = sc * (float)(int32_t)(s << 24);
    }
    for (; i < l; i++) {
        *seed = wa_mlow_smpl_rand(*seed);
        noise[i] = sc * (float)*seed;
    }
}

// squared-signal smoothing envelope (4-wide).
static void smpl_get_env(const float *exc, int length, float smth_coef, float *smth_state, float *env)
{
    smth_coef *= smth_coef;
    float state = *smth_state + 1e-8f;
    state *= state;
    float gain_coef = 1.0f - smth_coef;
    float smth_coef2 = smth_coef * smth_coef;
    float gain_smth_coef = gain_coef * smth_coef;
    int i = 0;
    for (; i + 3 < length; i += 4) {
        float tmp0 = exc[i] * exc[i] + exc[i + 1] * exc[i + 1];
        float tmp1 = exc[i + 2] * exc[i + 2] + exc[i + 3] * exc[i + 3];
        float y1 = gain_coef * tmp1 + gain_smth_coef * tmp0 + smth_coef2 * state;
        float y0 = gain_coef * tmp0 + smth_coef * state;
        env[i] = (float)sqrt((double)y0);
        env[i + 1] = env[i];
        env[i + 2] = (float)sqrt((double)y1);
        env[i + 3] = env[i + 2];
        state = y1;
    }
    *smth_state = env[length - 1];
}

// decaying envelope when there is no excitation to seed from.
static void smpl_get_env0(int length, float smth_coef, float *smth_state, float *env)
{
    float smth_coef2 = smth_coef * smth_coef;
    env[0] = (*smth_state + 1e-8f) * smth_coef;
    env[1] = env[0];
    for (int i = 2; i + 2 < length; i += 4) {
        env[i + 2] = env[i - 1] * smth_coef2;
        env[i + 3] = env[i + 2];
        env[i] = env[i - 1] * smth_coef;
        env[i + 1] = env[i];
    }
    env[length - 2] = env[length - 3] * smth_coef;
    env[length - 1] = env[length - 2];
    *smth_state = env[length - 1];
}

// smpl_filt_ma1 (coef_len=2). x != y.
static void smpl_filt_ma1(const float *x, int n, const float coef[2], float *state, float *y)
{
    if (coef[0] == 1.0f) {
        for (int k = 1; k < n; k++) y[k] = x[k] + coef[1] * x[k - 1];
    } else {
        for (int k = 0; k < n; k++) y[k] = coef[0] * x[k];
        for (int k = 1; k < n; k++) y[k] += coef[1] * x[k - 1];
    }
    y[0] = coef[0] * x[0] + coef[1] * (*state);
    *state = x[n - 1];
}

// smpl_filt_ar1 (coef[0]==1).
static void smpl_filt_ar1(const float *x, int n, const float coef[2], float *state, float *y)
{
    float ar1 = -coef[1];
    float ytmp = *state;
    for (int nn = 0; nn < n; nn++) { ytmp = x[nn] + ytmp * ar1; y[nn] = ytmp; }
    *state = ytmp;
}

// smpl_filt_arma1: MA1 then AR1, state {ma, ar}.
static void smpl_filt_arma1(const float *x, int n, const float coef_ma[2], const float coef_ar[2], float state[2], float *y)
{
    float tmp[NSF];
    float ma_state = state[0];
    smpl_filt_ma1(x, n, coef_ma, &ma_state, tmp);
    state[0] = ma_state;
    float ar_state = state[1];
    smpl_filt_ar1(tmp, n, coef_ar, &ar_state, y);
    state[1] = ar_state;
}

// smpl_filt_ma2 (coef_len=3, state_len=2). x != y.
static void smpl_filt_ma2(const float *x, int n, const float coef[3], float state[2], float *y)
{
    if (coef[0] == 1.0f) {
        for (int i = 1; i < n; i++) y[i] = x[i] + coef[1] * x[i - 1];
    } else {
        for (int i = 0; i < n; i++) y[i] = coef[0] * x[i];
        for (int i = 1; i < n; i++) y[i] += coef[1] * x[i - 1];
    }
    for (int i = 2; i < n; i++) y[i] += coef[2] * x[i - 2];
    y[0] = coef[0] * x[0] + coef[1] * state[0] + coef[2] * state[1];
    y[1] += coef[2] * state[0];
    state[0] = x[n - 1];
    state[1] = x[n - 2];
}

// spectral factorization of a 3-tap autocorrelation into a 3-tap MA.
static void smpl_spec_fact2(const float c_in[3], float a[3])
{
    float c[3] = { c_in[0], c_in[1], c_in[2] };
    c[0] += 1e-30f;
    float inv_c0 = 1.0f / c[0];
    float r2 = c[2] * inv_c0;
    float r1 = c[1] / (c[0] * (1.0f + r2));
    for (int iter = 0; iter < 2; iter++) {
        float v0 = 1.0f + r1 * r1 + r2 * r2;
        float v1 = r1 + r1 * r2;
        float s = -2.0f / v0;
        float da0 = s * r1;
        float da1 = s * r2;
        s = v0 * inv_c0;
        float e1 = s * c[1] - v1;
        float e2 = s * c[2] - r2;
        float r0 = 2.0f * r1 + v0 * da0;
        float r3 = 2.0f * r2 + v0 * da1;
        float rr00 = r0 * r0;
        float rr01 = r0 * r3;
        float rr11 = r3 * r3;
        float rcap1 = 1.0f + r2 + v1 * da0;
        float r4 = r1 + v1 * da1;
        rr00 += rcap1 * rcap1;
        rr01 += rcap1 * r4;
        rr11 += r4 * r4;
        float re0 = rcap1 * e1;
        float re1 = r4 * e1;
        float r2c = r2 * da0;
        float r5 = 1.0f + r2 * da1;
        rr00 += r2c * r2c;
        rr01 += r2c * r5;
        rr11 += r5 * r5;
        re0 += r2c * e2;
        re1 += r5 * e2;
        s = rr00 * rr11 - rr01 * rr01;
        if (s < 1e-4f) break;
        s = 1.0f / s;
        r1 += (rr11 * re0 - rr01 * re1) * s;
        r2 += (-rr01 * re0 + rr00 * re1) * s;
    }
    float sc = (float)sqrt((double)(c[0] / (1.0f + r1 * r1 + r2 * r2)));
    a[0] = sc;
    a[1] = sc * r1;
    a[2] = sc * r2;
}

// noise DCT matrix (dct_mat[CORR+1][DCT_ORDER]), built once.
static float g_dct[NCORR + 1][NDCT];
static int g_dct_ready;
static void noise_dct(void)
{
    if (g_dct_ready) return;
    float sc = 1.0f / (float)sqrt((double)NDCT);
    for (int i = 0; i < NDCT; i++) {
        float d_omega = ((0.5f + (float)i) * PI_NOISE) / (float)NDCT;
        float omega = 0.0f;
        for (int j = 0; j < NCORR + 1; j++) {
            g_dct[j][i] = (float)cos((double)omega) * sc;
            omega += d_omega;
        }
    }
    g_dct_ready = 1;
}

static void noise_mat_mult_transp16(float x[NCORR + 1], float *y, int len_x)
{
    float yt[NDCT];
    float xtmp = x[0];
    for (int i = 0; i < NDCT; i++) yt[i] = g_dct[0][i] * xtmp;
    for (int j = 1; j < len_x; j++) {
        float xt = x[j];
        for (int i = 0; i < NDCT; i++) yt[i] += g_dct[j][i] * xt;
    }
    memcpy(y, yt, sizeof(yt));
}

static void noise_mat_mult(const float *x, float *y)
{
    for (int i = 0; i < NCORR + 1; i++) {
        float acc = 0.0f;
        for (int k = 0; k < NDCT; k++) acc += g_dct[i][k] * x[k];
        y[i] = acc;
    }
}

float wa_mlow_norm_bitrate(int32_t num_pulses, int32_t frame_len16)
{
    float pulses_per_20ms = (float)(num_pulses * frame_len16) / (20.0f * 16.0f);
    return smpl_sigmoid(1.4f * (float)log2((double)(pulses_per_20ms + 1.0f)) - 6.5f);
}

float wa_mlow_decode_resnrg(int32_t nrgres_dbq_q14, int32_t fcb_subfrlen)
{
    float e = 0.1f * ((float)nrgres_dbq_q14 / (float)((int32_t)1 << 14));
    float resnrg = (float)pow(10.0, (double)e) - RESNRG_BIAS;
    if (resnrg < 0.0f) resnrg = 0.0f;
    return resnrg * (float)fcb_subfrlen;
}

// HP-shape the unvoiced noise and add it into noise.
static void add_noise_uv(wa_mlow_noise_gen *ng, float *exc_noise_uv, int l, const float *lsf, float nrg_ratio, float *noise)
{
    float lsf_hz = 16000.0f * (lsf[0] + lsf[1]) / (4.0f * PI_NOISE);
    float min_uv_fcorner = lsf_hz * 3.0f * smpl_sigmoid(0.2f / (lsf[1] - lsf[0] + 1e-30f) - 3.0f);
    float uv_fcorner = DEC_UV_FCORNER_HZ * minf(0.6f + 0.4f * nrg_ratio, 1.0f);
    uv_fcorner = maxf(uv_fcorner, min_uv_fcorner);
    uv_fcorner = minf(uv_fcorner, 1500.0f);
    float coef_tmp = 6.0f * uv_fcorner / 16000.0f;
    float g = (1.0f - 0.5f * coef_tmp) * DEC_UV_NOISE_GAIN;
    float coef_ma_uv[2] = { g, -g };
    float coef_ar_uv[2] = { 1.0f, -1.0f + coef_tmp };
    float filtered[NSF];
    smpl_filt_arma1(exc_noise_uv, l, coef_ma_uv, coef_ar_uv, ng->out_state_uv, filtered);
    memcpy(exc_noise_uv, filtered, (size_t)l * sizeof(float));
    for (int i = 0; i < l; i++) noise[i] += exc_noise_uv[i];
}

void wa_mlow_gen_noise(wa_mlow_noise_gen *ng, const float *exc_lpc, int l, int voiced, int32_t num_pulses,
                       float nrgres, int32_t fcbg_idx, const float *lsf, float normalized_bitrate,
                       const float *fcbgains_uv, float *noise)
{
    float nrg_ratio = 1.0f;
    float noise_uv[NSF], noise_v[NSF], noise_v2[NSF], env[NSF];

    if (voiced) {
        float corrs[NCORR + 1], c[NCORR + 1], ctgt[NCORR + 1];
        for (int i = 0; i < NCORR + 1; i++) {
            float acc = 0.0f;
            for (int k = 0; k < l - i; k++) acc += exc_lpc[k] * exc_lpc[k + i];
            corrs[i] = acc;
        }
        corrs[0] += 1e-12f;
        float corr_smth_coef = (l == FS_KHZ * 10) ? 0.4f : 0.16f;
        for (int i = 0; i < NCORR + 1; i++) ng->corr_smth[i] += corr_smth_coef * (corrs[i] - ng->corr_smth[i]);
        float scale = DEC_V_NOISE_GAIN * DEC_V_NOISE_GAIN * corrs[0] / ng->corr_smth[0];
        for (int i = 0; i < NCORR + 1; i++) c[i] = ng->corr_smth[i] * scale;
        c[1] *= 2.0f;
        c[2] *= 2.0f;

        noise_dct();
        float f2[NDCT], f2tgt[NDCT];
        noise_mat_mult_transp16(c, f2, NCORR + 1);
        float m = smpl_maximum(f2, NDCT) * 1.5f;
        for (int i = 0; i < NDCT; i++) f2tgt[i] = m - f2[i];
        noise_mat_mult(f2tgt, ctgt);
        smpl_gen_rand_pulses(noise_v, l, &ng->rand_seed);
        if (!ng->prev_voiced) ng->env_smth = ng->env_last;
        smpl_get_env(exc_lpc, l, ENV_SMTH_V, &ng->env_smth, env);
        for (int i = 0; i < l; i++) noise_v[i] *= env[i];
        float nrg_noise = smpl_nrg(noise_v, l);
        float inv = 1.0f / (nrg_noise + 1e-12f);
        for (int i = 0; i < NCORR + 1; i++) ctgt[i] *= inv;
        float coef_ma[NCORR + 1];
        smpl_spec_fact2(ctgt, coef_ma);
        smpl_filt_ma2(noise_v, l, coef_ma, ng->shape_state, noise_v2);

        if (!ng->prev_voiced) {
            smpl_gen_rand_pulses(noise_uv, l, &ng->rand_seed);
            float env_val = ng->env_last * ENV_SMTH_UVV;
            for (int i = 0; i < l; i += 2) {
                noise_uv[i] *= env_val;
                noise_uv[i + 1] *= env_val * ENV_SMTH_UVV;
                env_val *= ENV_SMTH_UVV * ENV_SMTH_UVV;
            }
        } else if (ng->since_unvoiced < 2) {
            for (int i = 0; i < l; i++) noise_uv[i] = 0.0f;
        }
        ng->env_last = env[l - 1];
    } else {
        for (int i = 0; i < NCORR + 1; i++) ng->corr_smth[i] = 0.0f;
        for (int i = 0; i < NCORR; i++) ng->shape_state[i] = 0.0f;
        for (int i = 0; i < l; i++) noise_v2[i] = 0.0f;

        float nrg_tgt;
        if (num_pulses > 0) {
            nrg_ratio = smpl_nrg(exc_lpc, l) / (nrgres + 1e-20f);
            float hardness = 10.0f + 20.0f * normalized_bitrate;
            nrg_tgt = nrgres * (float)log((double)((float)exp((double)(hardness * (1.0f - nrg_ratio))) + 1.0f)) / hardness;
            smpl_get_env(exc_lpc, l, ENV_SMTH_UV, &ng->env_smth, env);
        } else {
            nrg_ratio = 0.0f;
            nrg_tgt = nrgres;
            smpl_get_env0(l, ENV_SMTH_UV, &ng->env_smth, env);
        }

        float scale = 1.0f / (float)l;
        nrg_tgt = nrg_tgt * scale + 1e-30f;
        float nrg_env = smpl_nrg(env, l) * scale;
        float f = (float)sqrt((double)nrg_tgt);
        float gg = (float)sqrt((double)(nrg_tgt / nrg_env));
        float ge = gg * env[0];
        float env_last = ng->env_last;
        if (env_last < minf(f, ge)) {
            if (f < ge) gg = 0.0f; else f = 0.0f;
        } else if (env_last > maxf(f, ge)) {
            if (f > ge) gg = 0.0f; else f = 0.0f;
        } else {
            float sum_env = smpl_sum(env, l) * scale;
            float a = nrg_env + env[0] * env[0] - 2.0f * sum_env * env[0];
            float b = 2.0f * env_last * (sum_env - env[0]);
            float cc = env_last * env_last - nrg_tgt;
            float tmp = b * b - 4.0f * a * cc;
            if (tmp < 1e-35f || a < 1e-25f) {
                f = 0.0f;
                gg = 0.0f;
            } else {
                tmp = (float)sqrt((double)tmp);
                scale = 0.5f / a;
                gg = (-b + tmp) * scale;
                f = env_last - env[0] * gg;
                if (f < 0.0f) {
                    gg = (-b - tmp) * scale;
                    f = env_last - env[0] * gg;
                }
            }
        }

        smpl_gen_rand_pulses(noise_uv, l, &ng->rand_seed);
        if (num_pulses > 0) {
            float max_val = fcbgains_uv[fcbg_idx] * 0.5f;
            for (int i = 0; i < l; i++) {
                if (exc_lpc[i] == 0.0f) noise_uv[i] *= minf(f + gg * env[i], max_val);
                else noise_uv[i] = 0.0f;
            }
            ng->env_last = minf(f + gg * env[l - 1], max_val);
        } else {
            for (int i = 0; i < l; i++) noise_uv[i] *= f + gg * env[i];
            ng->env_last = f + gg * env[l - 1];
        }
    }

    if (ng->prev_voiced || voiced) {
        smpl_filt_ma2(noise_v2, l, COEF_MA_V, ng->out_state_v, noise);
    } else {
        for (int i = 0; i < l; i++) noise[i] = 0.0f;
    }
    if (ng->since_unvoiced < 2 || !voiced) {
        add_noise_uv(ng, noise_uv, l, lsf, nrg_ratio, noise);
    } else {
        ng->out_state_uv[0] = 0.0f;
        ng->out_state_uv[1] = 0.0f;
    }
    ng->prev_voiced = voiced;
    if (voiced) ng->since_unvoiced++;
    else ng->since_unvoiced = 0;
}

// --- KAT (gennoise_vectors.json): float inputs reproduced bit-exactly; noise checked at 1e-6 ---
static float bf(uint32_t u) { union { uint32_t u; float f; } x; x.u = u; return x.f; }

extern const int wa_mlow_gennoise_kat_len;
extern const int32_t wa_mlow_gennoise_kat_voiced[];
extern const int32_t wa_mlow_gennoise_kat_sfpulses[];
extern const int32_t wa_mlow_gennoise_kat_fcbgidx[];
extern const uint32_t wa_mlow_gennoise_kat_nrgres[];
extern const uint32_t wa_mlow_gennoise_kat_normbr[];
extern const int32_t wa_mlow_gennoise_kat_seedout[];
extern const uint32_t wa_mlow_gennoise_kat_in_envsmth[];
extern const uint32_t wa_mlow_gennoise_kat_in_envlast[];
extern const uint32_t wa_mlow_gennoise_kat_in_osuv[][2];
extern const uint32_t wa_mlow_gennoise_kat_in_osv[][2];
extern const uint32_t wa_mlow_gennoise_kat_in_corr[][3];
extern const uint32_t wa_mlow_gennoise_kat_in_shape[][2];
extern const int32_t wa_mlow_gennoise_kat_in_prevvoiced[];
extern const int32_t wa_mlow_gennoise_kat_in_sinceuv[];
extern const int32_t wa_mlow_gennoise_kat_in_seed[];
extern const uint32_t wa_mlow_gennoise_kat_out_envlast[];
extern const uint32_t wa_mlow_gennoise_kat_out_osuv[][2];
extern const uint32_t wa_mlow_gennoise_kat_out_osv[][2];
extern const uint32_t wa_mlow_gennoise_kat_excpre[][80];
extern const uint32_t wa_mlow_gennoise_kat_lsf[][16];
extern const uint32_t wa_mlow_gennoise_kat_noise[][80];

int wa_mlow_noise_selftest(void)
{
    float fcbgains_uv[91];
    for (int ix = 0; ix <= 90; ix++) fcbgains_uv[ix] = (float)pow(10.0, 0.05 * ((double)ix - 90.0));

    for (int n = 0; n < wa_mlow_gennoise_kat_len; n++) {
        wa_mlow_noise_gen ng;
        memset(&ng, 0, sizeof ng);
        ng.env_smth = bf(wa_mlow_gennoise_kat_in_envsmth[n]);
        ng.env_last = bf(wa_mlow_gennoise_kat_in_envlast[n]);
        for (int k = 0; k < 2; k++) { ng.out_state_uv[k] = bf(wa_mlow_gennoise_kat_in_osuv[n][k]); ng.out_state_v[k] = bf(wa_mlow_gennoise_kat_in_osv[n][k]); }
        for (int k = 0; k < 3; k++) ng.corr_smth[k] = bf(wa_mlow_gennoise_kat_in_corr[n][k]);
        for (int k = 0; k < 2; k++) ng.shape_state[k] = bf(wa_mlow_gennoise_kat_in_shape[n][k]);
        ng.prev_voiced = wa_mlow_gennoise_kat_in_prevvoiced[n];
        ng.since_unvoiced = wa_mlow_gennoise_kat_in_sinceuv[n];
        ng.rand_seed = wa_mlow_gennoise_kat_in_seed[n];

        float exc[80], lsf[16], noise[NSF];
        for (int i = 0; i < 80; i++) exc[i] = bf(wa_mlow_gennoise_kat_excpre[n][i]);
        for (int i = 0; i < 16; i++) lsf[i] = bf(wa_mlow_gennoise_kat_lsf[n][i]);

        wa_mlow_gen_noise(&ng, exc, 80, wa_mlow_gennoise_kat_voiced[n], wa_mlow_gennoise_kat_sfpulses[n],
                          bf(wa_mlow_gennoise_kat_nrgres[n]), wa_mlow_gennoise_kat_fcbgidx[n], lsf,
                          bf(wa_mlow_gennoise_kat_normbr[n]), fcbgains_uv, noise);

        if (ng.rand_seed != wa_mlow_gennoise_kat_seedout[n]) {
            wa_log("mlow gennoise %d: seed %d != %d", n, ng.rand_seed, wa_mlow_gennoise_kat_seedout[n]);
            return -1;
        }
        for (int i = 0; i < 80; i++) {
            float want = bf(wa_mlow_gennoise_kat_noise[n][i]);
            float d = noise[i] - want; if (d < 0) d = -d;
            if (d >= 1e-6f) { wa_log("mlow gennoise %d: noise[%d] %.7g != %.7g", n, i, noise[i], want); return -1; }
        }
        float d = ng.env_last - bf(wa_mlow_gennoise_kat_out_envlast[n]); if (d < 0) d = -d;
        if (d >= 1e-6f) { wa_log("mlow gennoise %d: env_last mismatch", n); return -1; }
        for (int k = 0; k < 2; k++) {
            float a = ng.out_state_uv[k] - bf(wa_mlow_gennoise_kat_out_osuv[n][k]); if (a < 0) a = -a;
            float b = ng.out_state_v[k] - bf(wa_mlow_gennoise_kat_out_osv[n][k]); if (b < 0) b = -b;
            if (a >= 1e-6f || b >= 1e-6f) { wa_log("mlow gennoise %d: out_state mismatch", n); return -1; }
        }
    }
    return 0;
}
