// The Noise_XX_25519_AESGCM_SHA256 handshake WhatsApp runs inside its WebSocket, and the transport ciphers it
// produces. Frames on the wire are 3-byte big-endian lengths followed by the payload; the very first frame of a
// connection is preceded by the "WA" header.
#ifndef WA_NOISE_H
#define WA_NOISE_H

#include <stdint.h>
#include <stddef.h>
#include "wa_util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t key[32];
    uint32_t counter;
} wa_noise_cipher;

typedef struct {
    uint8_t h[32], ck[32], k[32];
    uint32_t n;
    int has_key;
    uint8_t eph_priv[32], eph_pub[32];
    uint8_t server_static[32];
    // what the server's certificate chain said (0 when unchecked)
    int cert_ok;
} wa_noise_hs;

// The connection header sent before the first frame: "WA" 6 3
extern const uint8_t WA_CONN_HEADER[4];

// Starts the handshake and returns the ClientHello HandshakeMessage protobuf in out (unframed).
int wa_noise_start(wa_noise_hs *hs, wa_buf *out);
// Processes the server's HandshakeMessage (ServerHello) and builds the ClientFinish for our static key pair and the
// ClientPayload protobuf. Returns 0, or -1 (bad data / decryption failure / certificate problem in err).
int wa_noise_finish(wa_noise_hs *hs, const uint8_t *server_hello, size_t len, const uint8_t static_priv[32],
                    const uint8_t static_pub[32], const uint8_t *client_payload, size_t payload_len, wa_buf *out,
                    char *err, size_t errlen);
// Splits into the transport ciphers (call after wa_noise_finish)
void wa_noise_split(wa_noise_hs *hs, wa_noise_cipher *write, wa_noise_cipher *read);

// Transport: encrypt/decrypt one frame payload (AES-256-GCM, IV = 8 zero bytes + 32-bit counter)
int wa_noise_encrypt(wa_noise_cipher *c, const uint8_t *in, size_t len, wa_buf *out);
int wa_noise_decrypt(wa_noise_cipher *c, const uint8_t *in, size_t len, wa_buf *out);

// Frame helpers: appends the 3-byte length + data (and the connection header when first is set)
int wa_noise_frame(wa_buf *out, const uint8_t *data, size_t len, int first);
// Takes one complete frame from the front of in (returns 1 and the payload in out), 0 when incomplete, -1 on error
int wa_noise_unframe(wa_buf *in, wa_buf *out);

#ifdef __cplusplus
}
#endif
#endif
