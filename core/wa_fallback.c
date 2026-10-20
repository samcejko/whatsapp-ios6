#include "wa_fallback.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Helper: trim whitespace and newline from end of string
static void rtrim(char *str) {
    if (!str) return;
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r' || str[len - 1] == ' ' || str[len - 1] == '\t')) {
        str[--len] = '\0';
    }
}

// Helper: find and replace all occurrences of a substring
static void replace_all(char *str, const char *find, const char *replace) {
    if (!str || !find || !replace) return;
    if (strlen(find) == 0) return;

    char buffer[WA_FALLBACK_MAX_URL_LEN * 2];
    char *pos = str;
    char *write = buffer;
    size_t find_len = strlen(find);
    size_t replace_len = strlen(replace);
    size_t remaining = sizeof(buffer) - 1;

    while (*pos && remaining > 0) {
        char *found = strstr(pos, find);
        if (!found) {
            // Copy rest
            size_t copy_len = strlen(pos);
            if (copy_len > remaining) copy_len = remaining;
            memcpy(write, pos, copy_len);
            write += copy_len;
            remaining -= copy_len;
            break;
        }

        // Copy up to found
        size_t copy_len = found - pos;
        if (copy_len > remaining) copy_len = remaining;
        memcpy(write, pos, copy_len);
        write += copy_len;
        remaining -= copy_len;

        // Write replacement
        if (replace_len > remaining) replace_len = remaining;
        memcpy(write, replace, replace_len);
        write += replace_len;
        remaining -= replace_len;

        pos = found + find_len;
    }
    *write = '\0';
    strcpy(str, buffer);
}

int wa_fallback_load(const char *tsv_path, wa_fallback_list *out) {
    if (!tsv_path || !out) return 0;

    FILE *f = fopen(tsv_path, "r");
    if (!f) return 0;

    out->count = 0;
    char line[1024];

    while (fgets(line, sizeof(line), f) && out->count < WA_FALLBACK_MAX_PROVIDERS) {
        rtrim(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        wa_fallback_provider *p = &out->providers[out->count];

        // Parse TSV: provider<TAB>url_template<TAB>domain
        char *tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = '\0';
        char *tab2 = strchr(tab1 + 1, '\t');
        if (!tab2) continue;
        *tab2 = '\0';

        // Copy fields
        strncpy(p->provider_name, line, sizeof(p->provider_name) - 1);
        p->provider_name[sizeof(p->provider_name) - 1] = '\0';

        strncpy(p->url_template, tab1 + 1, sizeof(p->url_template) - 1);
        p->url_template[sizeof(p->url_template) - 1] = '\0';

        strncpy(p->domain, tab2 + 1, sizeof(p->domain) - 1);
        p->domain[sizeof(p->domain) - 1] = '\0';

        out->count++;
    }

    fclose(f);
    return out->count;
}

int wa_fallback_build_url(const wa_fallback_provider *provider,
                          const char *path, const char *query,
                          char *out_url, size_t out_size) {
    if (!provider || !out_url || out_size == 0) return 0;

    // Start with template
    strncpy(out_url, provider->url_template, out_size - 1);
    out_url[out_size - 1] = '\0';

    // Replace {PATH} placeholder
    if (path) {
        replace_all(out_url, "{PATH}", path);
    } else {
        replace_all(out_url, "{PATH}", "");
    }

    // Replace {QS} placeholder
    if (query) {
        replace_all(out_url, "{QS}", query);
    } else {
        replace_all(out_url, "{QS}", "");
    }

    return 1;
}