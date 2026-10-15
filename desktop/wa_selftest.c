// Offline self-tests for the WhatsApp core: crypto vectors (Curve25519/XEdDSA, HKDF, AES), the binary-node round
// trip, the protobuf writer/reader, and a client-payload sanity check. No network, no account - safe to run in CI.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "wa_util.h"
#include "wa_crypto.h"
#include "wa_proto.h"
#include "wa_node.h"
#include "wa_store.h"
#include "wa_srtp.h"
#include "wa_call.h"
#include "wa_wstun.h"
#include "wa_relay.h"
#include "wa_signal.h"
#include "wa_message.h"

static int g_fail;
static void check(const char *name, int ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) g_fail = 1;
}

static void log_sink(const char *line) { printf("  %s\n", line); }

static int test_proto(void)
{
    wa_buf b;
    wa_buf_init(&b);
    pb_put_varint(&b, 1, 300);
    pb_put_string(&b, 2, "hello");
    pb_put_bool(&b, 3, 1);
    wa_buf sub;
    wa_buf_init(&sub);
    pb_put_varint(&sub, 1, 42);
    pb_put_message(&b, 4, &sub);
    wa_buf_free(&sub);

    uint64_t v = 0;
    char *s = pb_get_string(b.data, b.len, 2);
    const uint8_t *m;
    size_t ml;
    uint64_t inner = 0;
    int ok = pb_get_varint(b.data, b.len, 1, &v) && v == 300 &&
             s && strcmp(s, "hello") == 0 &&
             pb_get_bytes(b.data, b.len, 4, &m, &ml) && pb_get_varint(m, ml, 1, &inner) && inner == 42;
    uint32_t path[2] = { 4, 1 };
    const uint8_t *p;
    size_t pl;
    ok = ok && pb_get_path(b.data, b.len, path, 1, &p, &pl);
    free(s);
    wa_buf_free(&b);
    return ok;
}

static int test_payload(void)
{
    wa_device d;
    if (wa_device_new(&d) != 0) return 0;
    wa_buf b;
    wa_buf_init(&b);
    int ok = wa_build_client_payload(&d, &b) == 0 && b.len > 32;
    // the registration payload must carry the device-pairing-data (field 19) with our identity key
    const uint8_t *reg, *ident;
    size_t reg_n, ident_n;
    ok = ok && pb_get_bytes(b.data, b.len, 19, &reg, &reg_n) &&
         pb_get_bytes(reg, reg_n, 3, &ident, &ident_n) && ident_n == 32 && memcmp(ident, d.identity_key.pub, 32) == 0;
    wa_buf_free(&b);

    // save/load round trip
    char tmp[L_tmpnam + 8];
    if (ok && tmpnam(tmp)) {
        wa_device d2;
        ok = wa_device_save(&d, tmp) == 0 && wa_device_load(&d2, tmp) == 0 &&
             d2.registration_id == d.registration_id && memcmp(d2.identity_key.priv, d.identity_key.priv, 32) == 0;
        remove(tmp);
    }
    return ok;
}

int main(void)
{
    wa_set_log_sink(log_sink);
    printf("WhatsApp6 core self-test\n");
    check("crypto (Curve25519/XEdDSA, HKDF, AES-GCM/CBC)", wa_crypto_selftest() == 0);
    check("binary node round trip", wa_node_selftest() == 0);
    check("protobuf writer/reader", test_proto());
    check("client payload + device store", test_payload());
    check("call SRTP (E2E keys, RTP IV, AES-CTR, WARP tag, ROC)", wa_srtp_selftest() == 0);
    check("call signaling stanzas (offer/accept/terminate)", wa_call_selftest() == 0);
    check("relay STUN (allocate/MI/fingerprint/ping/xor-endpoint)", wa_wstun_selftest() == 0);
    check("relay <relay> node parse (endpoints/tokens/hbh_key)", wa_relay_selftest() == 0);
    check("Signal session (X3DH + Double Ratchet, A<->B + out-of-order)", wa_signal_selftest() == 0);
    check("messaging (text + call-key: Message proto, Signal enc, <message> node)", wa_message_selftest() == 0);
    printf(g_fail ? "\nFAILED\n" : "\nALL PASS\n");
    return g_fail;
}
