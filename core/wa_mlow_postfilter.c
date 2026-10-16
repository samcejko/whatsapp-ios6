// MLow decoder-side postfilters (HP comb + per-packet harmonic). Faithful float32 port of meowcaller
// postfilter.go (whatsapp-rust smpl_harmcomb.rs + smpl_harm_postfilter.rs, MIT). Matches the -ffast-math
// reference to the i16 output LSB.
#include "wa_mlow_postfilter.h"
#include "wa_util.h"

#include <math.h>
#include <string.h>

#define INTF WA_PF_INTF_LEN
static const float PI_F32 = 3.1415927410125f;
static const float LO_EMPH1 = -0.995f;               // loEmph[1]
static const float LAG_CHANGE_THRESHOLD = 1.25f;

// HP curve constants
static const float HP_PITCH_MAF = 0.1f;
static const float HP_DEF_MAF = 0.1f;
static const float HP_DEF_FCORNER_HZ = 50.0f;
static const float HP_POSTF_TRANSITION_SPEED = 2.0f;
static const float HP_PITCH_ARF[2] = { 0.608057355f, 0.070939485f };
static const float HP_PITCH_ARR[2] = { -2.187380512f, 2.291030664f };
static const float HP_DEF_ARF[2] = { 0.728508218f, 0.476039848f };
static const float HP_DEF_ARR[2] = { -4.363803713f, 8.441854006f };

static float absf(float x) { return x < 0 ? -x : x; }
static float minf(float a, float b) { return a < b ? a : b; }
static float cos_approx(float x) { return 1.0f - 0.5f * x * x; }

// 3-tap FIR with carried 2-sample input history.
static void pf_fir3(const float *input, int n, const float coef[3], float state[2], float *out)
{
    float xm1 = state[0], xm2 = state[1];
    for (int i = 0; i < n; i++) {
        float p1 = (i >= 1) ? input[i - 1] : xm1;
        float p2 = (i >= 2) ? input[i - 2] : (i == 1 ? xm1 : xm2);
        out[i] = coef[0] * input[i] + coef[1] * p1 + coef[2] * p2;
    }
    if (n >= 2) { state[0] = input[n - 1]; state[1] = input[n - 2]; }
    else if (n == 1) { state[1] = xm1; state[0] = input[0]; }
}

// y[n] = in[n] - c1*y[n-1] - c2*y[n-2] (monic), 4-wide unrolled to match C rounding.
static void pf_filt_ar2(const float *input, int n, float c1, float c2, float state[2], float *out)
{
    float ytmp0 = state[1], ytmp1 = state[0];
    float ar1 = -c1, ar2 = -c2;
    float ar1_2 = ar1 * ar1, ar1_3 = ar1 * ar1_2, ar1_4 = ar1 * ar1_3;
    float imp1 = ar1, imp2 = ar1_2 + ar2, imp3 = ar1_3 + 2.0f * ar1 * ar2, imp4 = ar1_4 + ar2 * ar2 + 3.0f * ar1_2 * ar2;
    float ymp1 = ar2, ymp2 = ar2 * imp1, ymp3 = ar2 * imp2, ymp4 = ar2 * imp3;
    int nn = 0;
    for (; nn + 3 < n; nn += 4) {
        float xtmp0 = input[nn], xtmp1 = input[nn + 1], xtmp2 = input[nn + 2];
        out[nn + 2] = xtmp2 + imp1 * xtmp1 + imp2 * xtmp0 + imp3 * ytmp1 + ymp3 * ytmp0;
        float xtmp3 = input[nn + 3];
        out[nn + 3] = xtmp3 + imp1 * xtmp2 + imp2 * xtmp1 + imp3 * xtmp0 + imp4 * ytmp1 + ymp4 * ytmp0;
        out[nn] = xtmp0 + imp1 * ytmp1 + ymp1 * ytmp0;
        out[nn + 1] = xtmp1 + imp1 * xtmp0 + imp2 * ytmp1 + ymp2 * ytmp0;
        ytmp0 = out[nn + 2];
        ytmp1 = out[nn + 3];
    }
    for (; nn < n; nn++) {
        out[nn] = input[nn] + ar1 * ytmp1 + ar2 * ytmp0;
        ytmp0 = ytmp1;
        ytmp1 = out[nn];
    }
    state[1] = ytmp0;
    state[0] = ytmp1;
}

// leaky integrator y[n] = x[n] - c1*y[n-1], 5-wide unrolled.
static void pf_filt_ar1(const float *input, int n, float c1, float *state, float *out)
{
    float ar1 = -c1, ar1_2 = ar1 * ar1, ar1_3 = ar1 * ar1_2, ar1_4 = ar1 * ar1_3, ar1_5 = ar1 * ar1_4;
    float ytmp = *state;
    int nn = 0;
    for (; nn + 4 < n; nn += 5) {
        float x0 = input[nn], x1 = input[nn + 1], x2 = input[nn + 2], x3 = input[nn + 3], x4 = input[nn + 4];
        out[nn + 4] = x4 + ar1 * x3 + ar1_2 * x2 + ar1_3 * x1 + ar1_4 * x0 + ar1_5 * ytmp;
        out[nn] = x0 + ar1 * ytmp;
        out[nn + 1] = x1 + ar1 * x0 + ar1_2 * ytmp;
        out[nn + 2] = x2 + ar1 * x1 + ar1_2 * x0 + ar1_3 * ytmp;
        out[nn + 3] = x3 + ar1 * x2 + ar1_2 * x1 + ar1_3 * x0 + ar1_4 * ytmp;
        ytmp = out[nn + 4];
    }
    for (; nn < n; nn++) { ytmp = input[nn] + ytmp * ar1; out[nn] = ytmp; }
    *state = ytmp;
}

// y[n] = x[n] + c1*x[n-1] (companion pre-emphasis).
static void pf_filt_ma1(const float *input, int n, float c1, float *state, float *out)
{
    float prev = *state;
    for (int i = n - 1; i >= 1; i--) out[i] = input[i] + c1 * input[i - 1];
    if (n > 0) { out[0] = input[0] + c1 * prev; *state = input[n - 1]; }
}

// MA2 numerator then AR2 denominator, shared 4-wide state.
static void pf_filt_arma2(const float *input, int n, const float coef_ma[3], const float coef_ar[3], float state[4], float *out)
{
    float tmp[INTF];
    float ma_st[2] = { state[0], state[1] };
    pf_fir3(input, n, coef_ma, ma_st, tmp);
    state[0] = ma_st[0]; state[1] = ma_st[1];
    float ar_st[2] = { state[2], state[3] };
    pf_filt_ar2(tmp, n, coef_ar[1], coef_ar[2], ar_st, out);
    state[2] = ar_st[0]; state[3] = ar_st[1];
}

// unity-DC comb biquad.
static void calc_hp_coefs(float maf, const float arf[2], const float arr[2], float f, float coef_ma[3], float coef_ar[3])
{
    coef_ma[0] = 1.0f; coef_ma[1] = -2.0f * cos_approx(2.0f * PI_F32 * maf * f); coef_ma[2] = 1.0f;
    float far = arf[0] * f + arf[1] * f * f;
    float rar = arr[0] * f + arr[1] * f * f;
    coef_ar[0] = 1.0f;
    coef_ar[1] = -2.0f * cos_approx(2.0f * PI_F32 * far) * (1.0f + rar);
    coef_ar[2] = 1.0f + (2.0f * rar + rar * rar);
    float sc = (1.0f - coef_ar[1] + coef_ar[2]) / (1.0f - coef_ma[1] + coef_ma[2]);
    coef_ma[0] *= sc; coef_ma[1] *= sc; coef_ma[2] *= sc;
}

static void new_coefs(wa_mlow_hp_pf *st, float lag)
{
    if (lag > 0.0f) calc_hp_coefs(HP_PITCH_MAF, HP_PITCH_ARF, HP_PITCH_ARR, 1.0f / lag, st->coef_ma, st->coef_ar);
    else calc_hp_coefs(HP_DEF_MAF, HP_DEF_ARF, HP_DEF_ARR, HP_DEF_FCORNER_HZ / 16000.0f, st->coef_ma, st->coef_ar);
}

static float g_ramp_dn[INTF];
static int g_ramp_ready;
static void build_ramp_dn(void)
{
    if (g_ramp_ready) return;
    float d_omega = PI_F32 / (2.0f * ((float)INTF + 1.0f));
    float omega = d_omega;
    for (int i = 0; i < INTF; i++) {
        g_ramp_dn[i] = (float)pow((double)(float)cos((double)omega), (double)HP_POSTF_TRANSITION_SPEED);
        omega += d_omega;
    }
    g_ramp_ready = 1;
}

void wa_mlow_hp_pf_init(wa_mlow_hp_pf *st)
{
    memset(st, 0, sizeof(*st));
    st->lag_old = -1.0f;
}

void wa_mlow_hp_postfilter(wa_mlow_hp_pf *st, const float *xin, int n, float lag, float *out)
{
    float x[INTF];
    pf_filt_ar1(xin, n, LO_EMPH1, &st->state_lo_emph1, x);

    int overlap = 0;
    float y_old[INTF];
    if (st->lag_old < 0.0f) {
        new_coefs(st, lag);
        st->lag_old = lag;
    } else if (lag > LAG_CHANGE_THRESHOLD * st->lag_old || LAG_CHANGE_THRESHOLD * lag < st->lag_old) {
        overlap = 1;
        pf_filt_arma2(x, n, st->coef_ma, st->coef_ar, st->state_hp, y_old);
        new_coefs(st, lag);
        st->lag_old = lag;
        float x_old_copy[INTF];
        memcpy(x_old_copy, st->x_old, (size_t)n * sizeof(float));
        float dummy[INTF];
        pf_filt_arma2(x_old_copy, n, st->coef_ma, st->coef_ar, st->state_hp, dummy);
    } else if (lag != st->lag_old) {
        new_coefs(st, lag);
        st->lag_old = lag;
    }
    memcpy(st->x_old, x, (size_t)n * sizeof(float));

    float y_tmp[INTF];
    pf_filt_arma2(x, n, st->coef_ma, st->coef_ar, st->state_hp, y_tmp);

    if (overlap) {
        build_ramp_dn();
        for (int i = 0; i < n; i++) y_tmp[i] += (y_old[i] - y_tmp[i]) * g_ramp_dn[i];
    }
    pf_filt_ma1(y_tmp, n, LO_EMPH1, &st->state_lo_emph2, out);
}

// --- per-packet harmonic postfilter ---
#define HARM_MIN_PITCH_LAG 32
#define HARM_MAX_PITCH_LAG WA_HARM_MAX_PITCH_LAG
#define HARM_FB_DELAY WA_HARM_FB_DELAY
#define HARM_LAG_SUBFR_LEN 40
#define HARM_DELAY WA_HARM_DELAY
#define HARM_PITCH_NUM_SUBFRAMES 8
#define HARM_LP_FILT_RES 2500
#define HARM_NUM_LP_FILT (HARM_LP_FILT_RES / 80 - HARM_LP_FILT_RES / HARM_MAX_PITCH_LAG + 1)  // 25
static const float HARM_FB_STRENGTH = 0.4734f;
static const float HARM_STRENGTH = 0.6438f;
static const float HARM_CUTOFF_HZ = 4000.0f;
static const float HARM_NHARM_CUTOFF = 6.3f;
static const float HARM_REDUCTION_FAC = 0.0579f;

static int lag_to_filt_ix(int32_t lag)
{
    int32_t d = lag + 30;
    if (d < 80) d = 80;
    return (int)((int32_t)HARM_LP_FILT_RES / d - (int32_t)HARM_LP_FILT_RES / (int32_t)HARM_MAX_PITCH_LAG);
}

static float g_harm_lp[HARM_NUM_LP_FILT][2 * HARM_FB_DELAY + 1];
static int g_harm_ready;

static void harm_create_lp_filter(float omega0, const float filt_win[HARM_FB_DELAY], float blp[2 * HARM_FB_DELAY + 1])
{
    float omega_c = omega0 * HARM_NHARM_CUTOFF;
    float lim = HARM_CUTOFF_HZ / 16000.0f * PI_F32;
    if (lim < omega_c) omega_c = lim;
    float sum_b = 0.0f;
    float omega_c_sum = omega_c;
    for (int i = 0; i < HARM_FB_DELAY; i++) {
        float b = filt_win[i] * (float)sin((double)omega_c_sum);
        omega_c_sum += omega_c;
        blp[HARM_FB_DELAY + i + 1] = b;
        blp[HARM_FB_DELAY - i - 1] = b;
        sum_b += 2.0f * b;
    }
    blp[HARM_FB_DELAY] = omega_c;
    sum_b += omega_c;
    float sc = 1.0f / sum_b;
    for (int k = 0; k < 2 * HARM_FB_DELAY + 1; k++) blp[k] *= sc;
}

static void build_harm_tables(void)
{
    if (g_harm_ready) return;
    float filt_win[HARM_FB_DELAY];
    float d_omega = (0.5f * PI_F32) / ((float)HARM_FB_DELAY + 1.0f);
    float omega = d_omega;
    for (int i = 0; i < HARM_FB_DELAY; i++) { filt_win[i] = (float)cos((double)omega) / ((float)i + 1.0f); omega += d_omega; }
    int32_t ix_prev = -1;
    for (int32_t lag = HARM_MIN_PITCH_LAG; lag <= HARM_MAX_PITCH_LAG; lag++) {
        int32_t ix = (int32_t)lag_to_filt_ix(lag);
        if (ix != ix_prev) { harm_create_lp_filter(2.0f * PI_F32 / (float)lag, filt_win, g_harm_lp[ix]); ix_prev = ix; }
    }
    g_harm_ready = 1;
}

static float harm_dot(const float *a, const float *b, int l) { float r = 0; for (int i = 0; i < l; i++) r += a[i] * b[i]; return r; }
static float harm_nrg(const float *x, int n) { float r = 0; for (int i = 0; i < n; i++) r += x[i] * x[i]; return r; }

// 17-tap symmetric MA reading 16 samples of history from buf[xbase-16..].
static void harm_filt_ma16_sym(const float *buf, int xbase, int n, const float coef[17], float *out)
{
    for (int nn = 0; nn < n; nn++) {
        int c = xbase + nn;
        float res = buf[c - 8] * coef[8];
        for (int i = 0; i < 8; i++) res += coef[i] * (buf[c - i] + buf[c - 16 + i]);
        out[nn] = res;
    }
}

static void harm_postfilter_core(float lpcoefs[2 * HARM_FB_DELAY + 1], const float *comb, int comb_x, int32_t future_samples,
                                 int32_t lag, float *diff, int diff_base, float *out, int out_off, int l, float fb_strength,
                                 int32_t *prev_did_filter)
{
    int lag_u = (int)lag;
    float xy = 0.0f;
    if (lag > 0) {
        int32_t lookforward = (int32_t)l + lag - future_samples;
        if (lookforward > 0) {
            int l2 = (int)((int32_t)l - lookforward);
            if (l2 < 0) l2 = 0;
            for (int i = 0; i < l2; i++) out[out_off + i] = comb[comb_x + i - lag_u] + comb[comb_x + i + lag_u];
            for (int i = 0; i < l - l2; i++) out[out_off + l2 + i] = comb[comb_x + l2 + i - lag_u] + comb[comb_x + l2 + i];
        } else {
            for (int i = 0; i < l; i++) out[out_off + i] = comb[comb_x + i - lag_u] + comb[comb_x + i + lag_u];
        }
        xy = harm_dot(&comb[comb_x], &out[out_off], l);
    }
    if (lag > 0 && xy > 0.0f) {
        float xx = harm_nrg(&comb[comb_x], l);
        float yy = 0.25f * harm_nrg(&out[out_off], l);
        float denom = yy > xx ? yy : xx;
        float strength = 0.5f * xy / denom;
        float high_lag_reduction = 1.0f - HARM_REDUCTION_FAC * ((float)(lag - HARM_MIN_PITCH_LAG) / (float)(HARM_MAX_PITCH_LAG - HARM_MIN_PITCH_LAG));
        strength = strength * high_lag_reduction * HARM_STRENGTH;
        for (int i = 0; i < l; i++) out[out_off + i] *= 0.5f * strength;
        for (int i = 0; i < l; i++) diff[diff_base + i] = out[out_off + i] + (-strength) * comb[comb_x + i];
        const float *kernel = g_harm_lp[lag_to_filt_ix(lag)];
        for (int k = 0; k < 2 * HARM_FB_DELAY + 1; k++) lpcoefs[k] = kernel[k] * fb_strength;
        float yh[HARM_LAG_SUBFR_LEN];
        harm_filt_ma16_sym(diff, diff_base, l, lpcoefs, yh);
        for (int i = 0; i < l; i++) out[out_off + i] = yh[i] + comb[comb_x - HARM_FB_DELAY + i];
        *prev_did_filter = 1;
    } else {
        for (int i = 0; i < HARM_LAG_SUBFR_LEN; i++) diff[diff_base + i] = 0.0f;
        if (*prev_did_filter != 0) {
            float yh[2 * HARM_FB_DELAY];
            harm_filt_ma16_sym(diff, diff_base, 2 * HARM_FB_DELAY, lpcoefs, yh);
            for (int i = 0; i < 2 * HARM_FB_DELAY; i++) out[out_off + i] = yh[i] + comb[comb_x - HARM_FB_DELAY + i];
            for (int i = 2 * HARM_FB_DELAY; i < l; i++) out[out_off + i] = comb[comb_x + HARM_FB_DELAY + i - 2 * HARM_FB_DELAY];
        } else {
            for (int i = 0; i < l; i++) out[out_off + i] = comb[comb_x - HARM_FB_DELAY + i];
        }
        *prev_did_filter = 0;
    }
}

void wa_mlow_harm_pf_init(wa_mlow_harm_pf *st) { memset(st, 0, sizeof(*st)); }

void wa_mlow_harm_postfilter(wa_mlow_harm_pf *st, float *x, int xlen, const float *lags, int nlags, float normalized_bitrate)
{
    build_harm_tables();
    const int diff_prefix = 2 * HARM_FB_DELAY;   // 16
    float diff[INTF + 2 * HARM_FB_DELAY];
    memset(diff, 0, sizeof(diff));

    int32_t lag = st->prev_lag;
    int comb_cur = HARM_MAX_PITCH_LAG + HARM_DELAY;
    memcpy(&st->state_comb[comb_cur], x, (size_t)xlen * sizeof(float));

    float fb_strength = 1.0f - HARM_FB_STRENGTH * normalized_bitrate;
    int offset1 = 0, lag_ctr = 0;
    while (lag_ctr < nlags) {
        int offset2 = 0;
        memcpy(&diff[diff_prefix - 16], st->state1, sizeof(st->state1));
        int lag_ctr_end = lag_ctr + HARM_PITCH_NUM_SUBFRAMES;
        if (lag_ctr_end > nlags) lag_ctr_end = nlags;
        while (lag_ctr < lag_ctr_end) {
            int comb_x = HARM_MAX_PITCH_LAG + offset1;
            int32_t future_samples = (int32_t)HARM_DELAY + (int32_t)xlen - (int32_t)offset1;
            harm_postfilter_core(st->lpcoefs, st->state_comb, comb_x, future_samples, lag, diff, diff_prefix + offset2,
                                 x, offset1, HARM_LAG_SUBFR_LEN, fb_strength, &st->prev_did_filter);
            offset1 += HARM_LAG_SUBFR_LEN;
            offset2 += HARM_LAG_SUBFR_LEN;
            lag = (int32_t)lroundf(lags[lag_ctr]);
            lag_ctr++;
        }
        memcpy(st->state1, &diff[diff_prefix + offset2 - 16], sizeof(st->state1));
    }

    st->prev_lag = lag;
    memmove(&st->state_comb[0], &st->state_comb[xlen], (size_t)comb_cur * sizeof(float));
}

// --- KAT: HP (hp_postfilter_vectors.raw subset) + harm (first N packets, state carries) ---
static float bf(uint32_t u) { union { uint32_t u; float f; } x; x.u = u; return x.f; }

extern const int wa_mlow_hppf_kat_len;
extern const uint32_t wa_mlow_hppf_kat_lags[][8];
extern const uint32_t wa_mlow_hppf_kat_lo1[];
extern const uint32_t wa_mlow_hppf_kat_lo2[];
extern const uint32_t wa_mlow_hppf_kat_hp[][4];
extern const uint32_t wa_mlow_hppf_kat_lagold[];
extern const uint32_t wa_mlow_hppf_kat_xold[][320];
extern const uint32_t wa_mlow_hppf_kat_coefma[][3];
extern const uint32_t wa_mlow_hppf_kat_coefar[][3];
extern const uint32_t wa_mlow_hppf_kat_ypre[][320];
extern const uint32_t wa_mlow_hppf_kat_ypost[][320];

extern const int wa_mlow_harmpf_kat_len;
extern const int wa_mlow_harmpf_kat_plen[];
extern const int wa_mlow_harmpf_kat_nlags[];
extern const uint32_t wa_mlow_harmpf_kat_nbr[];
extern const int wa_mlow_harmpf_kat_lagoff[];
extern const uint32_t wa_mlow_harmpf_kat_lags[];
extern const int wa_mlow_harmpf_kat_sigoff[];
extern const uint32_t wa_mlow_harmpf_kat_inp[];
extern const uint32_t wa_mlow_harmpf_kat_cout[];

int wa_mlow_postfilter_selftest(void)
{
    const float i16lsb = 1.0f / 32768.0f;
    const float transition_tol = 6.0e-4f;

    // HP: each frame seeds its own state from the vector.
    for (int f = 0; f < wa_mlow_hppf_kat_len; f++) {
        wa_mlow_hp_pf st;
        memset(&st, 0, sizeof st);
        st.state_lo_emph1 = bf(wa_mlow_hppf_kat_lo1[f]);
        st.state_lo_emph2 = bf(wa_mlow_hppf_kat_lo2[f]);
        for (int i = 0; i < 4; i++) st.state_hp[i] = bf(wa_mlow_hppf_kat_hp[f][i]);
        st.lag_old = bf(wa_mlow_hppf_kat_lagold[f]);
        for (int i = 0; i < INTF; i++) st.x_old[i] = bf(wa_mlow_hppf_kat_xold[f][i]);
        for (int i = 0; i < 3; i++) { st.coef_ma[i] = bf(wa_mlow_hppf_kat_coefma[f][i]); st.coef_ar[i] = bf(wa_mlow_hppf_kat_coefar[f][i]); }

        float lag = 0.0f, l0 = bf(wa_mlow_hppf_kat_lags[f][0]);
        if (l0 > 0.0f) {
            float sl = 0, sll = 0;
            for (int i = 0; i < 8; i++) { float l = bf(wa_mlow_hppf_kat_lags[f][i]); sl += l; sll += l * l; }
            lag = sll / sl;
        }
        float ypre[INTF], out[INTF];
        for (int i = 0; i < INTF; i++) ypre[i] = bf(wa_mlow_hppf_kat_ypre[f][i]);
        wa_mlow_hp_postfilter(&st, ypre, INTF, lag, out);
        for (int i = 0; i < INTF; i++) {
            float d = absf(out[i] - bf(wa_mlow_hppf_kat_ypost[f][i]));
            if (d >= i16lsb) { wa_log("mlow hp pf %d: out[%d] diverges %.3e", f, i, d); return -1; }
        }
    }

    // harm: packets processed in order (state carries).
    wa_mlow_harm_pf st;
    wa_mlow_harm_pf_init(&st);
    float worst = 0.0f, worst_steady = 0.0f;
    const int tot_delay = HARM_FB_DELAY + HARM_DELAY;  // 48
    for (int p = 0; p < wa_mlow_harmpf_kat_len; p++) {
        int plen = wa_mlow_harmpf_kat_plen[p];
        int nlags = wa_mlow_harmpf_kat_nlags[p];
        float nbr = bf(wa_mlow_harmpf_kat_nbr[p]);
        float lags[64];
        int lo = wa_mlow_harmpf_kat_lagoff[p];
        for (int i = 0; i < nlags && i < 64; i++) lags[i] = bf(wa_mlow_harmpf_kat_lags[lo + i]);
        float inp[INTF * WA_HARM_MAX_FRAMES];
        int so = wa_mlow_harmpf_kat_sigoff[p];
        for (int i = 0; i < plen; i++) inp[i] = bf(wa_mlow_harmpf_kat_inp[so + i]);
        int transition = (bf(wa_mlow_harmpf_kat_lags[lo]) == 0.0f);
        wa_mlow_harm_postfilter(&st, inp, plen, lags, nlags, nbr);
        for (int i = 0; i < plen; i++) {
            float d = absf(inp[i] - bf(wa_mlow_harmpf_kat_cout[so + i]));
            if (d > worst) worst = d;
            if (!(transition && i < tot_delay) && d > worst_steady) worst_steady = d;
        }
    }
    if (worst_steady >= i16lsb) { wa_log("mlow harm pf: steady diverges %.3e", worst_steady); return -1; }
    if (worst >= transition_tol) { wa_log("mlow harm pf: transition residual %.3e", worst); return -1; }
    return 0;
}
