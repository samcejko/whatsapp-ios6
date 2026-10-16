// MLow CELP decoder-side noise generator (comfort/residual noise mixed into the CELP excitation). Float DSP,
// ported from meowcaller noise.go / whatsapp-rust smpl_gennoise.rs (MIT). Decoder-side only.
#ifndef WA_MLOW_NOISE_H
#define WA_MLOW_NOISE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WA_NOISE_MAX_SF_LEN   160
#define WA_NOISE_CORR_ORDER   2
#define WA_NOISE_DCT_ORDER    16

// Persistent decoder-side noise generator state (NoiseGenerator).
typedef struct {
    float env_smth, env_last;
    float out_state_uv[2], out_state_v[2];
    float corr_smth[WA_NOISE_CORR_ORDER + 1];
    float shape_state[WA_NOISE_CORR_ORDER];
    int prev_voiced;
    int32_t since_unvoiced;
    int32_t rand_seed;
} wa_mlow_noise_gen;

// smpl_RAND LCG (wrapping i32).
int32_t wa_mlow_smpl_rand(int32_t seed);
// normalized bitrate from a per-frame pulse count; residual-energy floor -> linear energy.
float wa_mlow_norm_bitrate(int32_t num_pulses, int32_t frame_len16);
float wa_mlow_decode_resnrg(int32_t nrgres_dbq_q14, int32_t fcb_subfrlen);

// Build l (<=160) samples of shaped residual noise into noise[]; updates ng. lsf length >= 2.
void wa_mlow_gen_noise(wa_mlow_noise_gen *ng, const float *exc_lpc, int l, int voiced, int32_t num_pulses,
                       float nrgres, int32_t fcbg_idx, const float *lsf, float normalized_bitrate,
                       const float *fcbgains_uv, float *noise);

int wa_mlow_noise_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
