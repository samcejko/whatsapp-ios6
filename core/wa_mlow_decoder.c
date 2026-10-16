// MLow top-level decoder. Faithful float32 port of meowcaller decoder.go + celpdec.go SynthFrame
// (whatsapp-rust decoder.rs + smpl_celpdec.rs, MIT).
#include "wa_mlow_decoder.h"
#include "wa_mlow_rc.h"
#include "wa_mlow_cc.h"
#include "wa_mlow_synth.h"
#include "wa_util.h"

#include <math.h>
#include <string.h>

#define INTF 320
#define ORD 16
#define SUBFR 80

// LSF->LPC interpolation factors per subframe [lsf_interpol_idx][sf].
static const float LSF_INTERPOL4[2][4] = { { 0.55f, 0.88f, 1.0f, 1.0f }, { 0.3f, 0.65f, 0.95f, 1.0f } };
static const float LSF_INTERPOL2[2][2] = { { 0.75f, 1.0f }, { 0.4f, 0.95f } };

typedef struct {
    int voiced;
    int32_t sf_pulses[4];
    int32_t fcbg_idx[4];
    int32_t nrgres_dbq_q14[4];
    int32_t acbg_idx[4];
    float block_lags[8];
    int32_t total_pulses;
} dec_params;

// per-subframe LSF interpolation + NLSF->A. mutates lsf_prev to the last interpolated LSF.
static void lpc_interpol(const float *nlsf, float *lsf_prev, const float *interpol, int num_subframes,
                         float a_out[4][ORD + 1], float lsfs_out[4][ORD])
{
    if (lsf_prev[ORD - 1] == 0.0f) memcpy(lsf_prev, nlsf, ORD * sizeof(float));
    float ilsf[ORD];
    memcpy(ilsf, nlsf, ORD * sizeof(float));   // default (used if first factor==prevFactor, unreachable)
    float prev_factor = -1.0f;
    for (int j = 0; j < num_subframes; j++) {
        if (interpol[j] == prev_factor) {
            memcpy(a_out[j], a_out[j - 1], (ORD + 1) * sizeof(float));
        } else {
            if (interpol[j] == 1.0f) memcpy(ilsf, nlsf, ORD * sizeof(float));
            else for (int k = 0; k < ORD; k++) ilsf[k] = lsf_prev[k] * (1.0f - interpol[j]) + nlsf[k] * interpol[j];
            wa_mlow_nlsf2a(ilsf, a_out[j]);
        }
        prev_factor = interpol[j];
        memcpy(lsfs_out[j], ilsf, ORD * sizeof(float));
    }
    memcpy(lsf_prev, ilsf, ORD * sizeof(float));
}

// y[n] = x[n] - sum_i a[16-i]*ybuf[base+n-16+i]; ybuf has a 16-sample history prefix at [base-16..base].
static void filt_ar16(float *x, const float a[ORD + 1], float *ybuf, int base, int n)
{
    for (int nn = 0; nn < n; nn++) {
        float res = x[nn];
        for (int i = 0; i < ORD; i++) res -= a[ORD - i] * ybuf[base + nn - ORD + i];
        ybuf[base + nn] = res;
    }
}

static void lowrate_uv_pulse_shape(float *x, int n, float state[2])
{
    float prev_x = state[0], prev_y = state[1];
    for (int i = 0; i < n; i++) {
        float ma = 0.5f * x[i] + 0.1665f * prev_x;
        prev_x = x[i];
        prev_y = ma + 0.333f * prev_y;
        x[i] = prev_y;
    }
    state[0] = prev_x;
    state[1] = prev_y;
}

static void lowrate_tilt(float *x, int n, float *state)
{
    float prev = *state;
    for (int i = 0; i < n; i++) { float s = x[i]; x[i] = 0.84f * s + 0.16f * prev; prev = s; }
    *state = prev;
}

// synthesize one 20 ms internal frame into out[320].
static void synth_frame(wa_mlow_decoder *d, const float *nlsf, int lsf_interpol_idx, const int32_t *pulses,
                        const dec_params *p, int low_rate, float *out)
{
    const float *gains_v = wa_mlow_fcb_gains_v();
    const float *gains_uv = wa_mlow_fcb_gains_uv();
    int idx = lsf_interpol_idx > 1 ? 1 : lsf_interpol_idx;
    if (idx < 0) idx = 0;
    int num_subframes = low_rate ? 2 : 4;
    int subframe_len = low_rate ? (INTF / 2) : SUBFR;
    const float *interpol = low_rate ? LSF_INTERPOL2[idx] : LSF_INTERPOL4[idx];

    float a[4][ORD + 1];
    float lsfs[4][ORD];
    lpc_interpol(nlsf, d->lsf_prev, interpol, num_subframes, a, lsfs);

    float norm_br = wa_mlow_norm_bitrate(p->total_pulses, INTF);

    float lpc_res[INTF];
    memset(lpc_res, 0, sizeof(lpc_res));
    const float *gain_tab = p->voiced ? gains_v : gains_uv;
    for (int pos = 0; pos < INTF; pos++) {
        if (pulses[pos] != 0) {
            int sf = pos / subframe_len;
            lpc_res[pos] = (float)pulses[pos] * gain_tab[p->fcbg_idx[sf]];
        }
    }

    int lags_per_subfr = subframe_len / 40;
    int acb_state_len = subframe_len + 2 * 320 + 8;
    float ybuf[ORD + INTF];
    memcpy(ybuf, d->lpc_synth_mem, ORD * sizeof(float));

    for (int sf = 0; sf < num_subframes; sf++) {
        int base = sf * subframe_len;
        const float *sf_lags = &p->block_lags[sf * lags_per_subfr];
        wa_mlow_celp_decode_subframe(d->acb.acb_state, acb_state_len, p->voiced, p->acbg_idx[sf], sf_lags,
                                     lags_per_subfr, subframe_len, low_rate, norm_br, &lpc_res[base]);

        if (low_rate && !p->voiced && p->sf_pulses[sf] > 0) lowrate_uv_pulse_shape(&lpc_res[base], subframe_len, d->uv_pulse_state);
        else { d->uv_pulse_state[0] = 0.0f; d->uv_pulse_state[1] = 0.0f; }

        float nrgres = wa_mlow_decode_resnrg(p->nrgres_dbq_q14[sf], (int32_t)subframe_len);
        if (!p->voiced) d->prev_nrgres = nrgres;
        float noise[160];
        wa_mlow_gen_noise(&d->noise, &lpc_res[base], subframe_len, p->voiced, p->sf_pulses[sf], nrgres,
                          p->fcbg_idx[sf], lsfs[sf], norm_br, gains_uv, noise);
        for (int i = 0; i < subframe_len; i++) lpc_res[base + i] += noise[i];

        if (low_rate && p->voiced) lowrate_tilt(&lpc_res[base], subframe_len, &d->tilt_state);
        else d->tilt_state = lpc_res[base + subframe_len - 1];

        filt_ar16(&lpc_res[base], a[sf], ybuf, ORD + base, subframe_len);
    }
    memcpy(out, &ybuf[ORD], INTF * sizeof(float));
    memcpy(d->lpc_synth_mem, &ybuf[ORD + INTF - ORD], ORD * sizeof(float));

    // per-frame HP comb (comb lag = energy-weighted mean of the 8 per-block lags; 0 unvoiced)
    float lag = 0.0f;
    if (p->voiced) {
        float sl = 0.0f, sll = 0.0f;
        for (int i = 0; i < 8; i++) { float l = p->block_lags[i]; sl += l; sll += l * l; }
        if (sl > 0.0f) lag = sll / sl;
    }
    float hp_out[INTF];
    wa_mlow_hp_postfilter(&d->hp, out, INTF, lag, hp_out);
    memcpy(out, hp_out, INTF * sizeof(float));
}

// decode one active 16 kHz / 60 ms MLow frame (3 internal frames) into out (960 samples).
static int decode_active_frame(wa_mlow_decoder *d, const uint8_t *frame, int len, float *out)
{
    if (wa_cc_init() != 0 || wa_cc_seed_init() != 0 || wa_mlow_pitch_init() != 0 || wa_mlow_synth_init() != 0) return 0;
    int config = (frame[0] >> 2) & 1;
    int low_rate = config;
    int num_subframes = low_rate ? 2 : 4;
    wa_rc dec;
    wa_rc_init(&dec, frame + 1, (size_t)(len - 1));

    float packet_lags[3 * 8];
    int npl = 0;
    float avg_norm_br = 0.0f;
    for (int f = 0; f < 3; f++) {
        wa_mlow_lsf_indices lsf;
        wa_mlow_decode_lsf(&dec, config, f, 1, &d->lstate, &lsf);
        wa_mlow_pulse_result pr;
        wa_mlow_decode_pulses(&dec, INTF, num_subframes, 1, config, lsf.stage1, &pr);
        int voiced = (lsf.stage1 == 1);
        dec_params p;
        memset(&p, 0, sizeof p);
        p.voiced = voiced;
        for (int k = 0; k < 4; k++) p.sf_pulses[k] = pr.subfr[k];
        int32_t total = 0;
        for (int k = 0; k < 4; k++) total += pr.subfr[k];
        p.total_pulses = total;
        if (voiced) {
            wa_mlow_pitch_result pit;
            wa_mlow_decode_pitch(&dec, &d->lstate, num_subframes, config, pr.subfr, &pit);
            for (int b = 0; b < 8; b++) {
                float v = (float)pit.block_lags[b] * 0.5f + 32.0f;
                if (v > 320.0f) v = 320.0f;
                p.block_lags[b] = v;
            }
            for (int sf = 0; sf < num_subframes; sf++) {
                p.acbg_idx[sf] = pit.gain_idx[sf];
                if (pit.filt_idx[sf] > 0) p.fcbg_idx[sf] = pit.filt_idx[sf];
            }
        } else {
            wa_mlow_gain_result g;
            wa_mlow_decode_gains(&dec, num_subframes, pr.subfr, &g);
            for (int sf = 0; sf < 4; sf++) { p.nrgres_dbq_q14[sf] = g.gain_q[sf]; p.fcbg_idx[sf] = g.nrg_res[sf]; }
        }
        for (int b = 0; b < 8; b++) packet_lags[npl++] = p.block_lags[b];
        avg_norm_br += wa_mlow_norm_bitrate(p.total_pulses, INTF);

        float nlsf[ORD];
        wa_mlow_reconstruct_nlsf((int)lsf.stage1, config, (int)lsf.grid, lsf.stage2,
                                 d->have_prev_nlsf ? d->prev_nlsf : NULL, nlsf);
        synth_frame(d, nlsf, (int)lsf.extra, pr.pulses, &p, low_rate, &out[f * INTF]);
        memcpy(d->prev_nlsf, nlsf, ORD * sizeof(float));
        d->have_prev_nlsf = 1;
    }

    wa_mlow_harm_postfilter(&d->harm, out, 3 * INTF, packet_lags, npl, avg_norm_br / 3.0f);
    for (int i = 0; i < 3 * INTF; i++) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }
    return 3 * INTF;
}

// decode one bare frame (TOC routing). Returns samples written, 0 for silence-len, -1 for std-Opus.
static int decode_frame(wa_mlow_decoder *d, const uint8_t *frame, int len, float *out, int max_out)
{
    if (len == 0) { int n = WA_MLOW_FRAME_SAMPLES; if (n > max_out) n = max_out; memset(out, 0, (size_t)n * sizeof(float)); return n; }
    wa_mlow_toc toc;
    wa_mlow_parse_toc(frame[0], &toc);
    int out_len = toc.std_opus ? (16 * toc.frame_ms) : (toc.sample_rate / 1000 * toc.frame_ms);
    if (toc.std_opus) return -1;   // caller decodes with libopus
    if (toc.sid || !toc.active) {
        int n = out_len; if (n <= 0 || n > max_out) n = (out_len > max_out ? max_out : (out_len > 0 ? out_len : 0));
        if (n > max_out) n = max_out;
        if (n < 0) n = 0;
        memset(out, 0, (size_t)n * sizeof(float));
        return n;
    }
    if (toc.sample_rate != 16000 || toc.frame_ms != 60 || len < 1) {
        int n = WA_MLOW_FRAME_SAMPLES; if (n > max_out) n = max_out;
        memset(out, 0, (size_t)n * sizeof(float));
        return n;
    }
    if (WA_MLOW_FRAME_SAMPLES > max_out) return 0;
    return decode_active_frame(d, frame, len, out);
}

void wa_mlow_decoder_init(wa_mlow_decoder *d) { memset(d, 0, sizeof(*d)); }

int wa_mlow_decode_payload(wa_mlow_decoder *d, const uint8_t *payload, int len, float *out, int max_out)
{
    if (len == 0) { int n = WA_MLOW_FRAME_SAMPLES; if (n > max_out) n = max_out; memset(out, 0, (size_t)n * sizeof(float)); return n; }
    // 0x92 SplitRed container: 0x92 <count> [ <len> <frame> ]*(count-1) <rest>
    if (len >= 3 && payload[0] == 0x92) {
        int count = payload[1];
        if (count >= 2 && count <= 8) {
            int off = 2, total = 0, ok = 1;
            int starts[8], lens[8], nsub = 0;
            for (int i = 0; i < count - 1 && ok; i++) {
                if (off >= len) { ok = 0; break; }
                int flen = payload[off]; off++;
                if (off + flen > len) { ok = 0; break; }
                starts[nsub] = off; lens[nsub] = flen; nsub++;
                off += flen;
            }
            if (ok && off < len) {
                starts[nsub] = off; lens[nsub] = len - off; nsub++;
                for (int i = 0; i < nsub; i++) {
                    int r = decode_frame(d, payload + starts[i], lens[i], out + total, max_out - total);
                    if (r < 0) r = 0;   // std-Opus sub-frame in a container: emit nothing (rare)
                    total += r;
                }
                return total;
            }
        }
    }
    return decode_frame(d, payload, len, out, max_out);
}

// --- e2e selftest: decode the embedded capture stream and correlate with the libopus useSmpl reference PCM.
// Mirrors TestE2EDecodeMatchesUseSmpl: length match + Pearson lag-0 correlation > 0.95 (synthesis is not
// bit-exact: PRNG noise + the reference's -ffast-math). Validates the whole synthesis chain at once. ---
extern const int wa_mlow_excpre_nframes;
extern const int wa_mlow_excpre_frameoff[];
extern const int wa_mlow_excpre_framelen[];
extern const uint8_t wa_mlow_excpre_frames[];
extern const int wa_mlow_e2e_ref_len;
extern const int16_t wa_mlow_e2e_ref[];

int wa_mlow_decoder_selftest(void)
{
    static float out[400000];
    wa_mlow_decoder d;
    wa_mlow_decoder_init(&d);
    int total = 0;
    for (int fr = 0; fr < wa_mlow_excpre_nframes; fr++) {
        const uint8_t *frame = &wa_mlow_excpre_frames[wa_mlow_excpre_frameoff[fr]];
        int flen = wa_mlow_excpre_framelen[fr];
        int r = wa_mlow_decode_payload(&d, frame, flen, out + total, (int)(sizeof(out) / sizeof(out[0])) - total);
        if (r < 0) r = 0;   // std-Opus frame: reference has audio here, we emit nothing (expected rare)
        total += r;
    }
    if (total != wa_mlow_e2e_ref_len) { wa_log("mlow e2e: length %d != ref %d", total, wa_mlow_e2e_ref_len); return -1; }

    double mr = 0, mo = 0;
    for (int i = 0; i < total; i++) { mr += (double)wa_mlow_e2e_ref[i] / 32768.0; mo += (double)out[i]; }
    mr /= total; mo /= total;
    double sxy = 0, sxx = 0, syy = 0;
    for (int i = 0; i < total; i++) {
        double dr = (double)wa_mlow_e2e_ref[i] / 32768.0 - mr;
        double dz = (double)out[i] - mo;
        sxy += dr * dz; sxx += dr * dr; syy += dz * dz;
    }
    double denom = sqrt(sxx * syy);
    if (denom == 0.0) { wa_log("mlow e2e: constant signal"); return -1; }
    double corr = sxy / denom;
    wa_log("mlow e2e lag-0 correlation vs useSmpl reference: %.4f", corr);
    if (corr <= 0.95) { wa_log("mlow e2e: corr %.4f <= 0.95", corr); return -1; }
    return 0;
}
