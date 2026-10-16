// MLow top-level: the TOC byte parse (per-frame routing between standard Opus and the MLow "smpl" codec) and the
// decode entry point. We send Opus; inbound frames are MLow by default, so the receive path must route per frame.
// The MLow smpl DSP (spectral/pitch/excitation/synthesis) is a staged port; std-Opus frames decode via libopus.
#ifndef WA_MLOW_H
#define WA_MLOW_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int std_opus;   // 1 = a standard Opus/CELT packet (decode with libopus), 0 = MLow smpl frame
    int sid;        // DTX / comfort-noise frame
    int vad;
    int sample_rate;
    int frame_ms;
    int fec;        // carries an in-band LBRR/FEC copy
    int active;     // coded as active voice
    int low_rate;
    int stereo;
} wa_mlow_toc;

// Parse the first payload byte.
void wa_mlow_parse_toc(uint8_t b, wa_mlow_toc *out);

// How a payload's first byte should be handled.
typedef enum { WA_MLOW_OPUS, WA_MLOW_SMPL, WA_MLOW_DTX } wa_mlow_route;
wa_mlow_route wa_mlow_route_of(uint8_t first_byte);

// --- MLow smpl decode (staged port) ---
#include "wa_mlow_rc.h"

// Cross-internal-frame LSF/predictor state.
typedef struct {
    int prev_stage1, prev_match, have_prev;
    int prev_gain_idx, prev_filt_idx, prev_lag, prev_frac_lag;
} wa_mlow_lsf_state;

typedef struct { int stage1, grid, stage2[16], extra; } wa_mlow_lsf_indices;

// Decode the LSF index block of one internal frame from the range coder. config 0/1, intf 0..2.
void wa_mlow_decode_lsf(wa_rc *dec, int config, int intf, int active, wa_mlow_lsf_state *st, wa_mlow_lsf_indices *out);

// Excitation pulses for one internal frame (p2 samples, p2 <= 320). pulses[] holds signed magnitudes per sample.
typedef struct { int32_t subfr[4]; int32_t pulses[320]; } wa_mlow_pulse_result;
void wa_mlow_decode_pulses(wa_rc *dec, int p2, int p3, int p4, int p6, int s1, wa_mlow_pulse_result *out);

// Per-subframe quantized log-gains + energy-residual symbols (unvoiced internal frames, LSF selector 0).
typedef struct { int32_t gain_q[4]; int32_t nrg_res[4]; int32_t gain_main, gain_delta; } wa_mlow_gain_result;
// p3 = subframe geometry (2 or 4); subfr[] = per-subframe pulse counts from the pulse stage.
void wa_mlow_decode_gains(wa_rc *dec, int p3, const int32_t subfr[4], wa_mlow_gain_result *out);

// Pitch / LTP parameters for one internal frame (voiced). lag/contour select the pitch contour; gain_idx/filt_idx
// are the per-subframe LTP gain/filter indices; int_lag_q6 is the per-subframe pitch lag in Q6 (1/64 sample).
typedef struct {
    int32_t gain_idx[4];
    int32_t filt_idx[4];
    int32_t lag, contour;
    int32_t int_lag_q6[4];
    int32_t sample_lag_q6[8];
    int32_t num_seg;
    int32_t block_lags[8];
    int32_t num_subfr;
} wa_mlow_pitch_result;

// Build the pitch lag/contour heap window + LTP tables (idempotent). Returns 0 or -1.
int wa_mlow_pitch_init(void);
// Decode LTP gains/filters + pitch lag/contour. p3 = num subframes, p6 = config (0 = active WB/HR),
// subfr[] = per-subframe pulse counts. st carries the cross-subframe predictor (prev gain/filt/lag).
void wa_mlow_decode_pitch(wa_rc *dec, wa_mlow_lsf_state *st, int p3, int p6, const int32_t subfr[4],
                          wa_mlow_pitch_result *res);

int wa_mlow_toc_selftest(void);
int wa_mlow_lsf_selftest(void);
int wa_mlow_pulse_selftest(void);
int wa_mlow_gains_selftest(void);
int wa_mlow_pitch_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
