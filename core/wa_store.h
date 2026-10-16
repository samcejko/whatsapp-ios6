// The device's long-lived identity: the key pairs and ids a linked device keeps across reconnects, and the state
// learned at pairing (our JIDs, the signed account identity). Serialized to a flat blob the platform stores in a
// file (iOS: the app container; desktop: next to the binary).
#ifndef WA_STORE_H
#define WA_STORE_H

#include <stdint.h>
#include <stddef.h>
#include "wa_util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t priv[32], pub[32];
} wa_keypair;

// One-time prekeys the companion uploads so peers (incl. our own phone) can open a Signal session to us.
// We keep the whole pool so an inbound PreKeySignalMessage that consumed id N still decrypts.
#define WA_OTPK_MAX 100
typedef struct {
    uint32_t id;
    wa_keypair key;
} wa_otpk;

typedef struct {
    uint32_t registration_id;         // 31-bit
    wa_keypair noise_key;             // the static key for the Noise handshake
    wa_keypair identity_key;          // Curve25519 identity (Signal)
    wa_keypair signed_pre_key;
    uint32_t signed_pre_key_id;
    uint8_t signed_pre_key_sig[64];
    uint8_t adv_secret_key[32];       // shared with the primary via the QR code; pairing HMAC key

    int registered;                   // 1 after a successful pairing
    char pn[64];                      // our phone JID ("4915...:3@s.whatsapp.net"), empty until paired
    char lid[64];                     // our LID JID
    char push_name[64];

    uint8_t account[2048];            // ADVSignedDeviceIdentity bytes (the signed account identity)
    size_t account_len;

    uint8_t server_static[32];        // cached server Noise static key (for future IK reconnect)
    int have_server_static;

    // one-time prekey pool (uploaded after login via <iq xmlns="encrypt" type="set">)
    wa_otpk one_time[WA_OTPK_MAX];
    int one_time_count;
    uint32_t next_pre_key_id;         // monotonic id source for new one-time prekeys (1-based, 24-bit)
    int server_has_prekeys;           // 1 once we've uploaded a batch at least once
} wa_device;

// Fills a brand-new device with fresh keys. 0 or -1.
int wa_device_new(wa_device *d);

// Flat serialization (versioned). Returns 0 or -1.
int wa_device_save(const wa_device *d, const char *path);
int wa_device_load(wa_device *d, const char *path);     // -1 when missing/corrupt (caller then makes a new one)

// Generate `count` fresh one-time prekeys, append them to the device pool (evicting the oldest when full),
// and write their public parts to out_ids[i]/out_pubs[i]. Returns the number generated (<= count, <= out_cap), or -1.
int wa_device_gen_prekeys(wa_device *d, int count, uint32_t *out_ids, uint8_t (*out_pubs)[32], int out_cap);
// Look up the private key of a stored one-time prekey by id. Returns the keypair or NULL if not held.
const wa_keypair *wa_device_find_prekey(const wa_device *d, uint32_t id);

// Builds the ClientPayload protobuf for the Noise handshake: a registration payload before pairing, a login
// payload once registered. Appends to out. Returns 0 or -1.
int wa_build_client_payload(const wa_device *d, wa_buf *out);

// The WhatsApp Web version this build advertises ("2.3000.x"); used for the registration build hash and version
// IQs. Overridable at run time once the live version is fetched.
const char *wa_client_version(void);
void wa_set_client_version(const char *v);

#ifdef __cplusplus
}
#endif
#endif
