#ifndef WA_FALLBACK_H
#define WA_FALLBACK_H

#include <stddef.h>

#define WA_FALLBACK_MAX_PROVIDERS 8
#define WA_FALLBACK_MAX_URL_LEN 512

typedef struct {
    char provider_name[32];      // "amazon", "google"
    char url_template[WA_FALLBACK_MAX_URL_LEN];  // URL with {PATH} and {QS} placeholders
    char domain[256];            // Host for SNI/headers
} wa_fallback_provider;

typedef struct {
    wa_fallback_provider providers[WA_FALLBACK_MAX_PROVIDERS];
    int count;
} wa_fallback_list;

// Load fallback providers from TSV file
// Returns number of providers loaded, 0 on error
int wa_fallback_load(const char *tsv_path, wa_fallback_list *out);

// Build URL from template by replacing {PATH} and {QS} placeholders
// Returns 1 on success, 0 on error (buffer too small)
int wa_fallback_build_url(const wa_fallback_provider *provider,
                          const char *path, const char *query,
                          char *out_url, size_t out_size);

#endif