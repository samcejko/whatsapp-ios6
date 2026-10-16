// MLow low-band synthesis: decoder-side NLSF reconstruction (from the stage indices + the lsf_seed codebook
// ROM) and the NLSF->LPC conversion. Float DSP ported from meowcaller synth.go + lsf_seed.go (whatsapp-rust
// smpl_synth.rs + smpl_lsf_seed.rs, MIT). Decoder subset only (encoder St1/St2 tables not built).
#ifndef WA_MLOW_SYNTH_H
#define WA_MLOW_SYNTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WA_SMPL_ORDER 16

// Build the decoder synthesis tables from the embedded lsf_seed ROM (idempotent). Returns 0 or -1.
int wa_mlow_synth_init(void);

// Rebuild the quantized NLSF (order 16) from the decoded stage indices + the previous frame's NLSF.
// stage1 = voicing selector (0/1), config = low-rate bit (0/1), grid = grid index (0..16), stage2[16] = the
// per-coefficient stage-2 symbols. prev_nlsf may be NULL (treated as zeros). Writes 16 values into out.
void wa_mlow_reconstruct_nlsf(int stage1, int config, int grid, const int32_t stage2[16],
                              const float *prev_nlsf, float *out);

// Convert an order-16 NLSF vector to the monic LPC coefficient vector a[0..16] (a[0]=1).
void wa_mlow_nlsf2a(const float *nlsf, float *a /* [17] */);

int wa_mlow_synth_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
