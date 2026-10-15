// Linked-device (companion) pairing. We show a QR built from our keys + a server ref; the user scans it with their
// phone's WhatsApp > Linked devices. The phone answers with a signed device identity we verify, counter-sign and
// send back.
#ifndef WA_PAIR_H
#define WA_PAIR_H

#include <stddef.h>
#include "wa_store.h"

#ifdef __cplusplus
extern "C" {
#endif

// The QR payload string for one <ref> (comma-separated: ref, our noise pub, our identity pub, our adv secret,
// client-type byte). malloc'd; NULL on failure. client_type: '1' = Chrome-like web client.
char *wa_pair_qr_string(const wa_device *d, const char *ref, char client_type);

// Runs the pairing crypto on the phone's ADVSignedDeviceIdentityHMAC (the <device-identity> content of
// pair-success): verifies the HMAC against our adv secret, verifies the phone's account signature, adds our own
// device signature, and returns the re-encoded self-signed identity to send back. On success it also stores the
// account identity into d->account. Returns 0 (and *key_index), or -1 with a reason in err.
int wa_pair_complete(wa_device *d, const uint8_t *device_identity, size_t len, uint8_t *out, size_t out_cap,
                     size_t *out_len, uint32_t *key_index, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
#endif
