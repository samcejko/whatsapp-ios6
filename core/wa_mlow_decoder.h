// MLow top-level decoder: RED/SplitRed strip -> TOC routing -> active-frame decode (3 chained 20 ms internal
// frames: LSF -> pulses -> pitch/gains -> NLSF reconstruct -> CELP synthesis) -> per-packet harmonic postfilter
// -> 60 ms PCM. Cross-frame predictor + synthesis history persist across calls. Ported from meowcaller
// decoder.go + celpdec.go SynthFrame (whatsapp-rust decoder.rs + smpl_celpdec.rs, MIT).
#ifndef WA_MLOW_DECODER_H
#define WA_MLOW_DECODER_H

#include <stdint.h>
#include "wa_mlow.h"
#include "wa_mlow_noise.h"
#include "wa_mlow_postfilter.h"
#include "wa_mlow_celp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WA_MLOW_FRAME_SAMPLES 960   // 60 ms @ 16 kHz

// Full cross-frame MLow decoder state.
typedef struct {
    wa_mlow_lsf_state lstate;        // LSF predictor + pitch predictor (prev gain/filt/lag)
    float prev_nlsf[16];
    int have_prev_nlsf;

    // CELP synthesis state
    wa_mlow_celp_state acb;          // ACB/LTP history
    wa_mlow_noise_gen noise;         // comfort-noise generator
    wa_mlow_hp_pf hp;                // per-frame HP comb
    float lsf_prev[16];              // lpcInterpol carry
    float lpc_synth_mem[16];         // filtAR16 history
    float uv_pulse_state[2];
    float tilt_state;
    float prev_nrgres;

    wa_mlow_harm_pf harm;            // per-packet harmonic postfilter
} wa_mlow_decoder;

void wa_mlow_decoder_init(wa_mlow_decoder *d);

// Decode one RTP MLow payload into PCM (float in [-1,1]) written to out (capacity max_out samples).
// Returns the number of samples written (0 on empty), or -1 if the payload is a standard-Opus frame the
// caller must hand to libopus instead. Handles the 0x92 SplitRed container and bare frames (redundancy 0).
int wa_mlow_decode_payload(wa_mlow_decoder *d, const uint8_t *payload, int len, float *out, int max_out);

int wa_mlow_decoder_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
