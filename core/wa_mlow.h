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

int wa_mlow_toc_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
