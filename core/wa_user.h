// usync (a contact's device list) and prekey-bundle fetch: the IQs a linked device sends before it can encrypt
// a message or a call key to a peer, and the parsers for their replies.
#ifndef WA_USER_H
#define WA_USER_H

#include <stdint.h>
#include <stddef.h>
#include "wa_node.h"
#include "wa_signal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WA_MAX_DEVICES 32

typedef struct {
    char jids[WA_MAX_DEVICES][80];   // full device JIDs ("user:dev@server")
    int ndevices;
} wa_device_list;

// Build the usync device-list IQ for one or more bare user JIDs ("<num>@s.whatsapp.net"). sid is a random id the
// caller supplies (16 hex). Returns a new <iq> node (no id attr; the client assigns it). Free with wa_node_free.
wa_node *wa_user_build_devices_iq(const char *const *user_jids, int n, const char *sid);
// Parse a usync response into the flat list of device JIDs.
int wa_user_parse_devices(const wa_node *iq_result, wa_device_list *out);

// Build the prekey-fetch IQ for device JIDs ("<num>:<dev>@s.whatsapp.net"). Returns a new <iq> node.
wa_node *wa_user_build_prekey_iq(const char *const *device_jids, int n);

// Build the one-time-prekey UPLOAD IQ (<iq xmlns="encrypt" type="set">): our registration id, identity, a batch
// of one-time prekeys (ids[]/pubs[]), and the signed prekey. This is what tells the server peers can open a session
// to us; without it the server has 0 keys and nothing is ever delivered. Returns a new <iq> node (no id attr).
wa_node *wa_user_build_upload_prekeys_iq(uint32_t reg_id, const uint8_t identity_pub[32],
                                         uint32_t skey_id, const uint8_t skey_pub[32], const uint8_t skey_sig[64],
                                         const uint32_t *ids, const uint8_t (*pubs)[32], int nkeys);
// Parse one <user> node from a prekey-fetch response into a bundle. Returns 1 on success.
int wa_user_parse_bundle(const wa_node *user_node, wa_sig_bundle *out);

int wa_user_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
