#include "wa_mlow_rc.h"
#include "wa_util.h"

#include <string.h>

#define EC_SYM_BITS 8
#define EC_CODE_BITS 32
#define EC_SYM_MAX 255u
#define EC_CODE_TOP (1u << 31)
#define EC_CODE_BOT (EC_CODE_TOP >> EC_SYM_BITS)   // 1<<23
#define EC_CODE_EXTRA 7u                            // (32-2)%8 + 1
#define EC_WINDOW_SIZE 32u
#define EC_UINT_BITS 8

static int32_t ilog(uint32_t x)
{
    int32_t n = 0;
    while (x) { n++; x >>= 1; }
    return n;   // floor(log2(x))+1 for x>0, 0 for x==0
}

static uint32_t read_byte(wa_rc *d)
{
    if (d->offs < d->storage) return d->buf[d->offs++];
    return 0;
}

static uint32_t read_byte_from_end(wa_rc *d)
{
    if (d->end_offs < d->storage) { d->end_offs++; return d->buf[d->storage - d->end_offs]; }
    return 0;
}

static void normalize(wa_rc *d)
{
    while (d->rng <= EC_CODE_BOT) {
        d->nbits_total += EC_SYM_BITS;
        d->rng <<= EC_SYM_BITS;
        int32_t sym0 = d->rem;
        d->rem = (int32_t)read_byte(d);
        int32_t sym = (sym0 << EC_SYM_BITS | d->rem) >> (EC_SYM_BITS - EC_CODE_EXTRA);
        d->val = ((d->val << EC_SYM_BITS) + (EC_SYM_MAX & ~(uint32_t)sym)) & (EC_CODE_TOP - 1);
    }
}

void wa_rc_init(wa_rc *d, const uint8_t *buf, size_t len)
{
    memset(d, 0, sizeof *d);
    d->buf = buf;
    d->storage = (uint32_t)len;
    d->nbits_total = EC_CODE_BITS + 1 - (int32_t)(((EC_CODE_BITS - EC_CODE_EXTRA) / EC_SYM_BITS) * EC_SYM_BITS);
    d->rng = 1u << EC_CODE_EXTRA;
    d->rem = (int32_t)read_byte(d);
    d->val = d->rng - 1 - ((uint32_t)(d->rem >> (EC_SYM_BITS - EC_CODE_EXTRA)));
    normalize(d);
}

uint32_t wa_rc_decode(wa_rc *d, uint32_t ft)
{
    if (ft == 0) { d->err = 1; d->ext = 1; return 0; }
    d->ext = d->rng / ft;
    if (d->ext == 0) { d->err = 1; d->ext = 1; return 0; }
    uint32_t s = d->val / d->ext;
    uint32_t s1 = s + 1 < ft ? s + 1 : ft;
    return ft - s1;
}

static uint32_t decode_bin(wa_rc *d, uint32_t bits_n)
{
    d->ext = d->rng >> bits_n;
    if (d->ext == 0) { d->err = 1; d->ext = 1; return 0; }
    uint32_t s = d->val / d->ext;
    uint32_t ft = 1u << bits_n;
    uint32_t s1 = s + 1 < ft ? s + 1 : ft;
    return ft - s1;
}

void wa_rc_update(wa_rc *d, uint32_t fl, uint32_t fh, uint32_t ft)
{
    uint32_t s = d->ext * (ft - fh);
    d->val -= s;
    if (fl > 0) d->rng = d->ext * (fh - fl);
    else d->rng -= s;
    normalize(d);
}

int wa_rc_bit_logp(wa_rc *d, uint32_t logp)
{
    uint32_t r = d->rng, dv = d->val, s = r >> logp;
    int ret = dv < s ? 1 : 0;
    if (ret == 0) { d->val = dv - s; d->rng = r - s; }
    else d->rng = s;
    normalize(d);
    return ret;
}

int wa_rc_decode_icdf(wa_rc *d, const uint8_t *icdf, size_t n, uint32_t ftb)
{
    if (n == 0) { d->err = 1; return 0; }
    uint32_t s0 = d->rng, dv = d->val, r = s0 >> ftb;
    int ret = -1;
    uint32_t t, s = s0;
    for (;;) {
        t = s;
        ret++;
        s = r * (uint32_t)icdf[ret];
        if (dv >= s || (size_t)ret >= n - 1) break;
    }
    d->val = dv - s;
    d->rng = t - s;
    normalize(d);
    return ret;
}

int wa_rc_decode_cdf(wa_rc *d, const uint16_t *cdf, size_t n)
{
    if (n < 2) { d->err = 1; return 0; }
    uint32_t base = cdf[0];
    if ((uint32_t)cdf[n - 1] <= base) { d->err = 1; return 0; }
    uint32_t ft = (uint32_t)cdf[n - 1] - base;
    uint32_t fs = wa_rc_decode(d, ft);
    uint32_t target = base + fs;
    size_t k = 0;
    while (k < n - 1) { if ((uint32_t)cdf[k + 1] > target) break; k++; }
    wa_rc_update(d, (uint32_t)cdf[k] - base, (uint32_t)cdf[k + 1] - base, ft);
    return (int)k;
}

int wa_rc_decode_cdf_window(wa_rc *d, const uint16_t *base, size_t base_len, int start, int n)
{
    if (n < 2) { d->err = 1; return 0; }
#define AT(i) ((start + (i)) >= 0 && (size_t)(start + (i)) < base_len ? (uint32_t)base[start + (i)] : 0u)
    uint32_t cbase = AT(0), last = AT(n - 1);
    if (last <= cbase) { d->err = 1; return 0; }
    uint32_t ft = last - cbase;
    uint32_t fs = wa_rc_decode(d, ft);
    uint32_t target = cbase + fs;
    int k = 0;
    while (k < n - 1) { if (AT(k + 1) > target) break; k++; }
    wa_rc_update(d, AT(k) - cbase, AT(k + 1) - cbase, ft);
#undef AT
    return k;
}

uint32_t wa_rc_decode_raw_symbol(wa_rc *d, uint32_t nbits)
{
    uint32_t sym = decode_bin(d, nbits);
    wa_rc_update(d, sym, sym + 1, 1u << nbits);
    return sym;
}

uint32_t wa_rc_bits_n(wa_rc *d, uint32_t n)
{
    uint32_t window = d->end_window;
    int32_t available = d->nend_bits;
    if ((uint32_t)available < n) {
        do {
            window |= read_byte_from_end(d) << (uint32_t)available;
            available += EC_SYM_BITS;
        } while ((uint32_t)available <= EC_WINDOW_SIZE - EC_SYM_BITS);
    }
    uint32_t ret = window & ((n >= 32 ? 0u : (1u << n)) - 1u);
    window >>= n;
    available -= (int32_t)n;
    d->end_window = window;
    d->nend_bits = available;
    d->nbits_total += (int32_t)n;
    return ret;
}

uint32_t wa_rc_decode_uint(wa_rc *d, uint32_t ft0)
{
    uint32_t ft = ft0 - 1;
    int32_t ftb = ilog(ft);
    if (ftb > EC_UINT_BITS) {
        ftb -= EC_UINT_BITS;
        uint32_t t = (ft >> (uint32_t)ftb) + 1;
        uint32_t s = wa_rc_decode(d, t);
        wa_rc_update(d, s, s + 1, t);
        uint32_t v = (s << (uint32_t)ftb) | wa_rc_bits_n(d, (uint32_t)ftb);
        if (v <= ft) return v;
        d->err = 1;
        return ft;
    }
    ft = ft + 1;
    uint32_t s = wa_rc_decode(d, ft);
    wa_rc_update(d, s, s + 1, ft);
    return s;
}

int wa_rc_decode_64_fine(wa_rc *d)
{
    d->ext = d->rng >> 6;
    if (d->ext == 0) { d->err = 1; d->ext = 1; return 0; }
    uint32_t s = d->val / d->ext;
    int64_t sym = 63 - (int64_t)s;
    if (sym < 0) sym = 0;
    if (sym > 64) sym = 64;
    wa_rc_update(d, (uint32_t)sym, (uint32_t)sym + 1, 64);
    return (int)sym;
}

int32_t wa_rc_tell(const wa_rc *d) { return d->nbits_total - ilog(d->rng); }

// -------------------------------------------------------------------------------------------------- KAT

// The vectors are generated into wa_mlow_rc_kat.c by tools/gen-rc-kat.ps1 from meowcaller's rc_vectors.json.
extern const uint8_t wa_rc_kat_bytes[];
extern const int wa_rc_kat_bytes_len;
extern const uint8_t wa_rc_kat_icdf[6];
extern const uint32_t wa_rc_kat_ops[][3];   // {kind, a, b}
extern const int wa_rc_kat_ops_len;
extern const uint8_t wa_rc_kat_cdf_bytes[];
extern const int wa_rc_kat_cdf_bytes_len;
extern const uint16_t wa_rc_kat_cdf_flat[];     // tables concatenated
extern const int wa_rc_kat_cdf_off[];           // offset of each table into _flat
extern const int wa_rc_kat_cdf_tablen[];        // length of each table
extern const int wa_rc_kat_cdf_ntables;
extern const uint32_t wa_rc_kat_cdf_ops[][2];   // {tableIndex, expectedA}
extern const int wa_rc_kat_cdf_ops_len;

int wa_mlow_rc_selftest(void)
{
    wa_rc d;
    wa_rc_init(&d, wa_rc_kat_bytes, (size_t)wa_rc_kat_bytes_len);
    for (int i = 0; i < wa_rc_kat_ops_len; i++) {
        uint32_t kind = wa_rc_kat_ops[i][0], a = wa_rc_kat_ops[i][1], b = wa_rc_kat_ops[i][2];
        uint32_t got;
        switch (kind) {
        case 0: got = (uint32_t)wa_rc_decode_icdf(&d, wa_rc_kat_icdf, 6, 8); if (got != a) { wa_log("rc kat op %d icdf got %u want %u", i, got, a); return -1; } break;
        case 1: got = wa_rc_bits_n(&d, a); if (got != b) { wa_log("rc kat op %d bits(%u) got %u want %u", i, a, got, b); return -1; } break;
        case 2: got = (uint32_t)wa_rc_bit_logp(&d, a); if (got != b) { wa_log("rc kat op %d bitlogp(%u) got %u want %u", i, a, got, b); return -1; } break;
        case 3: got = wa_rc_decode_uint(&d, a); if (got != b) { wa_log("rc kat op %d uint(%u) got %u want %u", i, a, got, b); return -1; } break;
        default: return -1;
        }
    }
    if (d.err) { wa_log("rc kat: decoder err after ops"); return -1; }

    wa_rc c;
    wa_rc_init(&c, wa_rc_kat_cdf_bytes, (size_t)wa_rc_kat_cdf_bytes_len);
    for (int i = 0; i < wa_rc_kat_cdf_ops_len; i++) {
        int ti = (int)wa_rc_kat_cdf_ops[i][0];
        uint32_t want = wa_rc_kat_cdf_ops[i][1];
        const uint16_t *tbl = &wa_rc_kat_cdf_flat[wa_rc_kat_cdf_off[ti]];
        int got = wa_rc_decode_cdf(&c, tbl, (size_t)wa_rc_kat_cdf_tablen[ti]);
        if ((uint32_t)got != want) { wa_log("rc kat cdf op %d table %d got %d want %u", i, ti, got, want); return -1; }
    }
    if (c.err) { wa_log("rc kat: cdf decoder err"); return -1; }
    return 0;
}
