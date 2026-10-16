// MLow CELP decoder excitation (the pre-noise excitation: FCB pulses x gain + voiced ACB/LTP synthesis).
// Float DSP ported from meowcaller celpdec.go (whatsapp-rust smpl_celpdec.rs, MIT). This is the excitation
// the CELP synthesis filter runs on; it is captured + KAT-verified before the noise/LPC stages, which do not
// feed back into it.
#ifndef WA_MLOW_CELP_H
#define WA_MLOW_CELP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WA_CELP_INTF_LEN 320
#define WA_CELP_SUBFR_LEN 80
#define WA_CELP_SUBFR_COUNT 4
#define WA_CELP_MAX_PITCH_LAG 320
#define WA_CELP_LTP_INTERPOL_DELAY 8
#define WA_CELP_ACB_STATE_LEN (160 + 2 * WA_CELP_MAX_PITCH_LAG + WA_CELP_LTP_INTERPOL_DELAY)  // 808

// Per-subframe decoded params the excitation consumes.
typedef struct {
    int voiced;
    int32_t fcbg_idx[WA_CELP_SUBFR_COUNT];   // FCB gain-table index per subframe
    int32_t acbg_idx[WA_CELP_SUBFR_COUNT];   // ACB gain index (voiced)
    float block_lags[8];                      // per-40-sample-block pitch lag (codec units)
    int32_t total_pulses;
} wa_mlow_celp_params;

// Persistent CELP decoder excitation state (the ACB/LTP history buffer).
typedef struct {
    float acb_state[WA_CELP_ACB_STATE_LEN];
} wa_mlow_celp_state;

void wa_mlow_celp_init(wa_mlow_celp_state *st);

// Build the pre-noise excitation for one 20 ms internal frame (4 or - low-rate - 2 subframes) into exc[320].
// pulses are the signed FCB pulse magnitudes (320 positions); low_rate is the TOC bit.
void wa_mlow_celp_exc_pre(wa_mlow_celp_state *st, const int32_t *pulses, const wa_mlow_celp_params *params,
                          int low_rate, float *exc);

// --- shared with the SynthFrame integrator (wa_mlow_decoder.c) ---
// FCB gain tables (built on first call): v = voiced (34 entries), uv = unvoiced (91 entries).
const float *wa_mlow_fcb_gains_v(void);
const float *wa_mlow_fcb_gains_uv(void);
// One subframe of the CELP decode: add voiced ACB/LTP into lpc_res, then push it into acb_state. num_lags =
// subfrlen/40. norm_br = packet normalized bitrate. Mirrors celpdec.go celpDecode.
void wa_mlow_celp_decode_subframe(float *acb_state, int acb_state_len, int voiced, int32_t acb_gain_idx,
                                  const float *lags, int num_lags, int subfrlen, int low_rate, float norm_br,
                                  float *lpc_res);

int wa_mlow_celp_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
