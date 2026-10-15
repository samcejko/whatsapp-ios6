// A desktop command-line harness that pairs as a linked device and prints incoming stanzas. It is for the user to
// run themselves on their own machine against their own account - it is NOT run in CI (CI only runs the offline
// self-tests). It prints the QR string; render it with any `qrencode -t ANSI` or an online text-to-QR to scan.
//
//   wa_pair_cli [store-file]      pair (first run) or reconnect, then print stanzas until Ctrl+C
//
// The store file (default wa6.store) holds the device identity + pairing result. Delete it to pair fresh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "wa_util.h"
#include "wa_crypto.h"
#include "wa_store.h"
#include "wa_client.h"

static wa_client *g_client;
static const char *g_store = "wa6.store";

static void on_sigint(int s) { (void)s; if (g_client) wa_client_stop(g_client); }

static void log_sink(const char *line) { fprintf(stderr, "%s\n", line); }

static void on_qr(void *ctx, const char *qr)
{
    (void)ctx;
    printf("\n=== SCAN THIS IN WhatsApp > Linked devices > Link a device ===\n");
    printf("QR data: %s\n", qr);
    printf("Render it, e.g.:  qrencode -t ANSIUTF8 '%s'\n", qr);
    printf("==============================================================\n\n");
}

static void on_paired(void *ctx, const char *jid)
{
    (void)ctx;
    printf(">>> PAIRED as %s. Reconnecting with the login session...\n", jid);
}

static void on_connected(void *ctx)
{
    wa_client *c = (wa_client *)ctx;
    printf(">>> CONNECTED and logged in.\n");
    wa_device d;   // persist the (now registered) device
    (void)d;
    (void)c;
}

static void on_device_changed(void *ctx, wa_device *d)
{
    (void)ctx;
    if (wa_device_save(d, g_store) == 0) printf(">>> device saved to %s\n", g_store);
}

static void on_node(void *ctx, const wa_node *node)
{
    (void)ctx;
    char *s = wa_node_to_string(node, 0);
    printf("<< %s", s ? s : "?\n");
    free(s);
}

static void on_disconnect(void *ctx, const char *reason, int reconnect)
{
    (void)ctx;
    printf(">>> disconnected: %s%s\n", reason, reconnect ? " (will reconnect)" : "");
}

int main(int argc, char **argv)
{
    wa_set_log_sink(log_sink);
    if (argc > 1) g_store = argv[1];
    if (wa_crypto_selftest() != 0) { fprintf(stderr, "crypto self-test failed; aborting\n"); return 1; }

    wa_device dev;
    if (wa_device_load(&dev, g_store) == 0) printf("loaded device from %s (registered=%d)\n", g_store, dev.registered);
    else { printf("new device\n"); if (wa_device_new(&dev) != 0) { fprintf(stderr, "key generation failed\n"); return 1; } wa_device_save(&dev, g_store); }

    wa_client_callbacks cb = { 0 };
    cb.on_qr = on_qr;
    cb.on_paired = on_paired;
    cb.on_connected = on_connected;
    cb.on_node = on_node;
    cb.on_disconnect = on_disconnect;
    cb.on_device_changed = on_device_changed;

    wa_client_config cfg = { 0 };
    cfg.ca_file = argc > 2 ? argv[2] : NULL;      // optional PEM bundle for TLS verification
    cfg.verify_tls = cfg.ca_file != NULL;

    g_client = wa_client_new(&dev, &cb, &cfg);
    signal(SIGINT, on_sigint);
    wa_client_run(g_client);
    wa_device_save(&dev, g_store);
    wa_client_free(g_client);
    return 0;
}
