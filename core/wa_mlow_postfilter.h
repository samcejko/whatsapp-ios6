// MLow decoder-side postfilters: the post-LPC HP (pitch-harmonic) comb applied per 20 ms frame, and the
// per-packet harmonic postfilter. Float DSP ported from meowcaller postfilter.go / whatsapp-rust
// smpl_harmcomb.rs + smpl_harm_postfilter.rs (MIT). The reference C is -ffast-math, so these match to the
// i16 output LSB (1/32768) rather than bit-exactly - identical once written to 16-bit PCM.
#ifndef WA_MLOW_POSTFILTER_H
#define WA_MLOW_POSTFILTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WA_PF_INTF_LEN 320

// --- HP (pitch-harmonic) comb, per 20 ms frame ---
typedef struct {
    float state_lo_emph1;
    float state_lo_emph2;
    float state_hp[4];     // [ma2 x[-1], x[-2], ar2 y[-1], y[-2]]
    float lag_old;
    float x_old[WA_PF_INTF_LEN];
    float coef_ma[3];
    float coef_ar[3];
} wa_mlow_hp_pf;

void wa_mlow_hp_pf_init(wa_mlow_hp_pf *st);
// Apply the HP comb. lag = frame average pitch lag (sum(l^2)/sum(l)), 0 for unvoiced. out != xin.
void wa_mlow_hp_postfilter(wa_mlow_hp_pf *st, const float *xin, int n, float lag, float *out);

// --- per-packet harmonic postfilter ---
#define WA_HARM_MAX_PITCH_LAG 320
#define WA_HARM_DELAY 40
#define WA_HARM_FB_DELAY 8
#define WA_HARM_MAX_FRAMES 6
#define WA_HARM_STATE_COMB_LEN (WA_HARM_MAX_PITCH_LAG + WA_PF_INTF_LEN * WA_HARM_MAX_FRAMES + WA_HARM_DELAY)

typedef struct {
    float state1[2 * WA_HARM_FB_DELAY];
    float lpcoefs[2 * WA_HARM_FB_DELAY + 1];
    float state_comb[WA_HARM_STATE_COMB_LEN];
    int32_t prev_lag;
    int32_t prev_did_filter;
} wa_mlow_harm_pf;

void wa_mlow_harm_pf_init(wa_mlow_harm_pf *st);
// Apply the harmonic postfilter to a full packet IN PLACE. lags = per-40-block lags (nlags = xlen/40).
void wa_mlow_harm_postfilter(wa_mlow_harm_pf *st, float *x, int xlen, const float *lags, int nlags, float normalized_bitrate);

int wa_mlow_postfilter_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
