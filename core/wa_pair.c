#include "wa_pair.h"
#include "wa_crypto.h"
#include "wa_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ADV signature prefixes (WAWebAdvSignatureConstants)
static const uint8_t ADV_ACCOUNT_PREFIX[2] = { 6, 0 };
static const uint8_t ADV_DEVICE_PREFIX[2] = { 6, 1 };
static const uint8_t ADV_HOSTED_ACCOUNT_PREFIX[2] = { 6, 5 };

// ADVEncryptionType: 0 = E2EE (default), 1 = HOSTED
#define ADV_HOSTED 1

char *wa_pair_qr_string(const wa_device *d, const char *ref, char client_type)
{
    char noise[48], ident[48], adv[48];
    wa_base64_encode(d->noise_key.pub, 32, noise, sizeof noise);
    wa_base64_encode(d->identity_key.pub, 32, ident, sizeof ident);
    wa_base64_encode(d->adv_secret_key, 32, adv, sizeof adv);
    return wa_sprintf("%s,%s,%s,%s,%c", ref, noise, ident, adv, client_type);
}

int wa_pair_complete(wa_device *d, const uint8_t *device_identity, size_t len, uint8_t *out, size_t out_cap,
                     size_t *out_len, uint32_t *key_index, char *err, size_t errlen)
{
    // 1. ADVSignedDeviceIdentityHMAC { details(1), hmac(2), accountType(3) }
    const uint8_t *details, *hmac;
    size_t details_n, hmac_n;
    if (!pb_get_bytes(device_identity, len, 1, &details, &details_n) ||
        !pb_get_bytes(device_identity, len, 2, &hmac, &hmac_n)) {
        snprintf(err, errlen, "device-identity HMAC container incomplete");
        return -1;
    }
    uint64_t acct_type = 0;
    pb_get_varint(device_identity, len, 3, &acct_type);
    int hosted = acct_type == ADV_HOSTED;

    // 2. HMAC-SHA256(adv_secret, [hosted prefix] + details) must equal hmac
    uint8_t mac[32];
    if (hosted) {
        const void *parts[2] = { ADV_HOSTED_ACCOUNT_PREFIX, details };
        size_t lens[2] = { 2, details_n };
        wa_hmac_sha256_v(d->adv_secret_key, 32, 2, parts, lens, mac);
    } else {
        wa_hmac_sha256(d->adv_secret_key, 32, details, details_n, mac);
    }
    if (hmac_n != 32 || wa_ct_compare(mac, hmac, 32) != 0) {
        snprintf(err, errlen, "pairing HMAC mismatch");
        return -1;
    }

    // 3. ADVSignedDeviceIdentity { details(1), accountSignatureKey(2), accountSignature(3), deviceSignature(4) }
    const uint8_t *inner, *acct_key, *acct_sig;
    size_t inner_n, acct_key_n, acct_sig_n;
    if (!pb_get_bytes(details, details_n, 1, &inner, &inner_n) ||
        !pb_get_bytes(details, details_n, 2, &acct_key, &acct_key_n) ||
        !pb_get_bytes(details, details_n, 3, &acct_sig, &acct_sig_n) ||
        acct_key_n != 32 || acct_sig_n != 64) {
        snprintf(err, errlen, "signed device identity incomplete");
        return -1;
    }

    // ADVDeviceIdentity { rawId(1), timestamp(2), keyIndex(3), accountType(4), deviceType(5) }
    uint64_t ki = 0, device_type = 0;
    pb_get_varint(inner, inner_n, 3, &ki);
    pb_get_varint(inner, inner_n, 4, &device_type);

    // 4. account signature message = account_prefix + inner + our identity pub(32); verify with acct_key
    const uint8_t *acct_prefix = (device_type == ADV_HOSTED) ? ADV_HOSTED_ACCOUNT_PREFIX : ADV_ACCOUNT_PREFIX;
    wa_buf msg;
    wa_buf_init(&msg);
    wa_buf_append(&msg, acct_prefix, 2);
    wa_buf_append(&msg, inner, inner_n);
    wa_buf_append(&msg, d->identity_key.pub, 32);
    if (wa_curve_verify(acct_key, msg.data, msg.len, acct_sig) != 0) {
        wa_buf_free(&msg);
        snprintf(err, errlen, "account signature invalid");
        return -1;
    }

    // 5. our device signature = sign(identity priv, device_prefix + inner + identity pub(32) + acct_key(32))
    memcpy(msg.data, ADV_DEVICE_PREFIX, 2);
    wa_buf_append(&msg, acct_key, 32);
    uint8_t dev_sig[64];
    int rc = wa_curve_sign(dev_sig, d->identity_key.priv, msg.data, msg.len);
    wa_buf_free(&msg);
    if (rc != 0) {
        snprintf(err, errlen, "cannot sign device identity");
        return -1;
    }

    // 6. re-encode ADVSignedDeviceIdentity with details + accountSignatureKey + accountSignature + deviceSignature
    wa_buf signed_id;
    wa_buf_init(&signed_id);
    pb_put_bytes(&signed_id, 1, inner, inner_n);
    pb_put_bytes(&signed_id, 2, acct_key, acct_key_n);
    pb_put_bytes(&signed_id, 3, acct_sig, acct_sig_n);
    pb_put_bytes(&signed_id, 4, dev_sig, 64);

    if (signed_id.len > out_cap || signed_id.len > sizeof d->account) {
        wa_buf_free(&signed_id);
        snprintf(err, errlen, "signed identity too large");
        return -1;
    }
    memcpy(out, signed_id.data, signed_id.len);
    *out_len = signed_id.len;
    memcpy(d->account, signed_id.data, signed_id.len);
    d->account_len = signed_id.len;
    *key_index = (uint32_t)ki;
    wa_buf_free(&signed_id);
    return 0;
}
