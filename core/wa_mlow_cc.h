// MLow pulse-coding CDF tables (the split and run-length CMFs), computed from the SILK fixed-point model at init
// — not seed-built, so no ROM needed. Ported from meowcaller cc_tables (MIT). Integer/deterministic.
#ifndef WA_MLOW_CC_H
#define WA_MLOW_CC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WA_CC_MAX_PULSES_PER_SF 40
#define WA_CC_RUNLENGTH_STEP 8
#define WA_CC_NUM_RUNLEN 20
#define WA_CC_SPLIT_NUM_TABLES (WA_CC_MAX_PULSES_PER_SF * 4 - 1)   // 159

// Build the tables (idempotent). Returns 0 or -1.
int wa_cc_init(void);

// The split CMF for `total` pulses (1-based). Returns NULL/0 when out of range.
const uint16_t *wa_cc_split_cmf(int total, size_t *len);
// The run-length CMF for octave bucket `oct` (1..20) and `c` pulses (1..40); max_samples via wa_cc_runlen_max.
const uint16_t *wa_cc_runlen_cmf(int oct, int c, size_t *len);
int wa_cc_runlen_max(int oct);

int wa_mlow_cc_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
