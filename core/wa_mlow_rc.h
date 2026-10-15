// Opus/CELT range decoder (ec_dec, RFC 6716 §4.1), the entropy coder MLow reuses verbatim. Decoder only (we
// decode inbound MLow audio; we send Opus). Ported from the whatsapp-rust mlow rangecoder, KAT-verified against
// the meowcaller rc_vectors.
#ifndef WA_MLOW_RC_H
#define WA_MLOW_RC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *buf;
    uint32_t storage, end_offs, end_window;
    int32_t nend_bits, nbits_total;
    uint32_t offs, rng, val, ext;
    int32_t rem;
    int err;
} wa_rc;

void wa_rc_init(wa_rc *d, const uint8_t *buf, size_t len);
uint32_t wa_rc_decode(wa_rc *d, uint32_t ft);
void wa_rc_update(wa_rc *d, uint32_t fl, uint32_t fh, uint32_t ft);
int wa_rc_bit_logp(wa_rc *d, uint32_t logp);
int wa_rc_decode_icdf(wa_rc *d, const uint8_t *icdf, size_t n, uint32_t ftb);
int wa_rc_decode_cdf(wa_rc *d, const uint16_t *cdf, size_t n);
uint32_t wa_rc_bits_n(wa_rc *d, uint32_t n);
uint32_t wa_rc_decode_uint(wa_rc *d, uint32_t ft0);
int wa_rc_decode_64_fine(wa_rc *d);
int32_t wa_rc_tell(const wa_rc *d);

int wa_mlow_rc_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
