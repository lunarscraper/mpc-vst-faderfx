/* =============================================================================
 * djfx.c - "Fader FX": Octatrack-style performance effects for the MPC OS plugin
 * host, as an mpc-vst-plugins engine (wrapper/engine.h, built with "effect": true).
 *
 * One FADER morphs everything: at 0 the signal is untouched, pushing it right blends
 * every enabled effect from nothing to its scene setting, pulling it back returns to
 * the origin. Each effect's amount is  a = fader x its own depth  (only if it's ON).
 *
 * Chain:  in -> REPEAT -> DROP -> CRUSH -> FILTER -> GATER -> (+ RISER) -> out
 *                                                         \-> DELAY send -> + wet
 *                                                         \-> REVERB send -> + wet
 * Delay and reverb are sends: their tails keep ringing after the fader is back at 0
 * (a "throw"). Reverb FREEZE holds the tail forever while the fader is fully right.
 *
 * Tempo comes from the host (HAS_LFO_BPM -> "lfo_bpm"). Fixed 44.1 kHz, 128-frame
 * int16 stereo blocks, per the engine contract. Everything is allocated at create().
 * ========================================================================== */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

#define SR 44100.0f
#define TWO_PI 6.28318530718f

#define REC_LEN   (1 << 17)            /* 2.97 s input history (repeat, drop) */
#define DLY_LEN   (1 << 17)            /* 2.97 s delay line */
#define FDN_N     4
#define FDN_MAX   4096

/* ---- parameters (UI units, as the wrapper sends them) ------------------------ */
typedef struct { const char *key; float v; } pv_t;
enum {
    P_FADER, P_FLT_ON, P_FLT, P_FLT_RES, P_RPT_ON, P_RPT_DIV, P_CRUSH_ON, P_CRUSH,
    P_GATE_ON, P_GATE, P_GATE_DIV, P_RISE_ON, P_RISE, P_DROP_ON, P_DROP,
    P_DLY_ON, P_DLY, P_DLY_DIV, P_DLY_FB, P_REV_ON, P_REV, P_REV_SIZE, P_REV_FREEZE, P_COUNT
};
static const pv_t DEFAULTS[P_COUNT] = {
    {"fader", 0}, {"flt_on", 1}, {"flt", 70}, {"flt_res", 30}, {"rpt_on", 0}, {"rpt_div", 2},
    {"crush_on", 0}, {"crush", 70}, {"gate_on", 0}, {"gate", 100}, {"gate_div", 2},
    {"rise_on", 0}, {"rise", 60}, {"drop_on", 0}, {"drop", 80},
    {"dly_on", 1}, {"dly", 60}, {"dly_div", 2}, {"dly_fb", 50},
    {"rev_on", 0}, {"rev", 60}, {"rev_size", 70}, {"rev_freeze", 0},
};

typedef struct { float ic1, ic2; } svf_t;   /* TPT state-variable filter state, one channel */

typedef struct {
    float p[P_COUNT];
    float bpm;
    float fader_s;                         /* smoothed fader 0..1 */
    uint32_t rng;

    /* input history for repeat and drop */
    float rec[2][REC_LEN];
    uint32_t wpos;

    /* repeat */
    int rpt_active, rpt_level;
    uint32_t rpt_start, rpt_len, rpt_pos;
    float rpt_mix;                         /* 0..1 crossfade dry <-> repeat */

    /* drop (varispeed from the history) */
    double drop_lag;
    float drop_xf;                         /* >0 while crossfading back to live */
    float drop_lp[2];

    /* crush */
    float crush_hold[2];
    float crush_cnt;

    /* filter + riser band-pass */
    svf_t flt[2], rise_bp[2];
    float flt_amt_s, flt_res_s;

    /* gater */
    double gate_phase;
    float gate_g;

    /* delay */
    float dly[2][DLY_LEN];
    uint32_t dpos;
    float dly_time_s, dly_lp[2];

    /* reverb: 4-line FDN */
    float fdn[FDN_N][FDN_MAX];
    int fdn_pos[FDN_N];
    float fdn_lp[FDN_N];
    float rev_size_s;

    char state[1024];
} djfx_t;

static const int FDN_BASE[FDN_N] = {1117, 1453, 1777, 2111};

static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static float noise(djfx_t *d) {                  /* xorshift32, -1..1 */
    uint32_t x = d->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    d->rng = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}
/* one TPT SVF step: mode 0 = low-pass, 1 = high-pass, 2 = band-pass */
static float svf(svf_t *s, float x, float g, float k, int mode) {
    float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float v3 = x - s->ic2;
    float v1 = a1 * s->ic1 + a2 * v3;
    float v2 = s->ic2 + a2 * s->ic1 + a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    if (mode == 0) return v2;
    if (mode == 2) return v1;
    return x - k * v1 - v2;
}
static float frac_of_div(int idx, const float *table, int n) { return table[idx < 0 ? 0 : idx >= n ? n - 1 : idx]; }
static float beat_samples(djfx_t *d) { return SR * 60.0f / (d->bpm > 20 ? d->bpm : 120.0f); }   /* one quarter note */
static float rd(const float *buf, uint32_t mask, double pos) {   /* linear-interpolated read */
    uint32_t i = (uint32_t)pos;
    float f = (float)(pos - (double)i);
    return buf[i & mask] * (1.0f - f) + buf[(i + 1) & mask] * f;
}

/* ---- engine API --------------------------------------------------------------- */
static void *create(const char *data_dir) {
    (void)data_dir;
    djfx_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    for (int i = 0; i < P_COUNT; i++) d->p[i] = DEFAULTS[i].v;
    d->bpm = 120.0f;
    d->rng = 0x9E3779B9u;
    d->gate_g = 1.0f;
    d->rev_size_s = d->p[P_REV_SIZE] / 100.0f;
    d->flt_amt_s = 0.0f;
    return d;
}
static void destroy(void *inst) { free(inst); }
static void midi(void *inst, const uint8_t *msg, int len) { (void)inst; (void)msg; (void)len; }

static int find_key(const char *key) {
    for (int i = 0; i < P_COUNT; i++) if (!strcmp(DEFAULTS[i].key, key)) return i;
    return -1;
}
static void set_param(void *inst, const char *key, const char *val) {
    djfx_t *d = inst;
    if (!strcmp(key, "lfo_bpm")) { float b = (float)atof(val); if (b > 20 && b < 400) d->bpm = b; return; }
    if (!strcmp(key, "state")) {                    /* "key=value;..." from our own get_param */
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", val);
        char *save = NULL;
        for (char *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
            char *eq = strchr(t, '=');
            if (!eq) continue;
            *eq = 0;
            int i = find_key(t);
            if (i >= 0) d->p[i] = (float)atof(eq + 1);
        }
        return;
    }
    int i = find_key(key);
    if (i >= 0) d->p[i] = (float)atof(val);
}
static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    djfx_t *d = inst;
    if (!strcmp(key, "state")) {
        int n = 0;
        for (int i = 0; i < P_COUNT && n < buf_len - 40; i++)
            n += snprintf(buf + n, buf_len - n, "%s=%g;", DEFAULTS[i].key, d->p[i]);
        return n;
    }
    int i = find_key(key);
    if (i < 0) return -1;
    return snprintf(buf, buf_len, "%g", d->p[i]);
}
static void render(void *inst, int16_t *out, int frames) { (void)inst; memset(out, 0, sizeof(int16_t) * 2 * frames); }

static const float RPT_FRAC[4]  = {1.0f, 0.5f, 0.25f, 0.125f};          /* 1/4 .. 1/32 in quarters */
static const float GATE_FRAC[4] = {1.0f, 0.5f, 0.25f, 0.125f};
static const float DLY_FRAC[6]  = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};  /* 1/16 1/8 1/8. 1/4 1/4. 1/2 */

static void process(void *inst, const int16_t *in, int16_t *out, int frames) {
    djfx_t *d = inst;
    const float *p = d->p;
    const float qn = beat_samples(d);

    /* per-block targets */
    float fader_t = clampf(p[P_FADER] / 100.0f, 0, 1);
    for (int n = 0; n < frames; n++) {
        d->fader_s += (fader_t - d->fader_s) * 0.0015f;       /* ~15 ms glide: no zipper on a jumpy Q-Link */
        const float f = d->fader_s;
        float l = in[2 * n] * (1.0f / 32768.0f), r = in[2 * n + 1] * (1.0f / 32768.0f);

        /* history for repeat / drop */
        d->rec[0][d->wpos & (REC_LEN - 1)] = l;
        d->rec[1][d->wpos & (REC_LEN - 1)] = r;
        d->wpos++;

        /* ---- REPEAT: roll the last slice; pushing further = shorter slices ---- */
        if (p[P_RPT_ON] > 0.5f) {
            int maxlev = (int)p[P_RPT_DIV];                  /* finest division reached at full fader */
            if (maxlev < 0) maxlev = 0;
            if (maxlev > 3) maxlev = 3;
            int want = f < 0.08f ? -1 : (int)((f - 0.08f) / 0.92f * (maxlev + 1));
            if (want > maxlev) want = maxlev;
            if (want >= 0 && (!d->rpt_active || want != d->rpt_level)) {
                d->rpt_len = (uint32_t)(qn * RPT_FRAC[want]);
                if (d->rpt_len < 256) d->rpt_len = 256;
                if (d->rpt_len > REC_LEN / 2) d->rpt_len = REC_LEN / 2;
                if (!d->rpt_active) d->rpt_start = d->wpos - d->rpt_len;    /* capture the slice just played */
                /* a finer level keeps looping inside the slice already captured (classic roll) */
                d->rpt_pos = 0;
                d->rpt_level = want;
                d->rpt_active = 1;
            } else if (want < 0) d->rpt_active = 0;
        } else d->rpt_active = 0;
        d->rpt_mix += ((d->rpt_active ? 1.0f : 0.0f) - d->rpt_mix) * 0.01f;   /* ~2 ms crossfade */
        if (d->rpt_mix > 0.0005f && d->rpt_len) {
            uint32_t i0 = d->rpt_start + d->rpt_pos;
            float rl = d->rec[0][i0 & (REC_LEN - 1)], rr = d->rec[1][i0 & (REC_LEN - 1)];
            const uint32_t xf = 96;                                    /* loop-seam crossfade */
            if (d->rpt_pos >= d->rpt_len - xf) {
                float t = (float)(d->rpt_pos - (d->rpt_len - xf)) / xf;
                uint32_t i1 = d->rpt_start + (d->rpt_pos - (d->rpt_len - xf));
                rl = rl * (1 - t) + d->rec[0][i1 & (REC_LEN - 1)] * t;
                rr = rr * (1 - t) + d->rec[1][i1 & (REC_LEN - 1)] * t;
            }
            if (++d->rpt_pos >= d->rpt_len) d->rpt_pos = xf < d->rpt_len ? xf : 0;
            l = l * (1 - d->rpt_mix) + rl * d->rpt_mix;
            r = r * (1 - d->rpt_mix) + rr * d->rpt_mix;
        }

        /* ---- DROP: varispeed slow-down (tape-stop feel), darker as it slows ---- */
        float a_drop = p[P_DROP_ON] > 0.5f ? f * p[P_DROP] / 100.0f : 0.0f;
        if (a_drop > 0.01f) {
            float rate = 1.0f - a_drop * 0.97f;
            d->drop_lag += 1.0 - rate;
            if (d->drop_lag > REC_LEN - 4096) d->drop_lag = REC_LEN - 4096;
            d->drop_xf = 1.0f;
        }
        if (d->drop_lag > 0.0) {
            double pos = (double)(d->wpos - 1) - d->drop_lag;
            float dl = rd(d->rec[0], REC_LEN - 1, pos), dr = rd(d->rec[1], REC_LEN - 1, pos);
            float c = 0.05f + 0.95f * (1.0f - a_drop) * (1.0f - a_drop);
            d->drop_lp[0] += (dl - d->drop_lp[0]) * c;
            d->drop_lp[1] += (dr - d->drop_lp[1]) * c;
            if (a_drop <= 0.01f) {                               /* fader back: fade to live, then reset */
                d->drop_xf -= 1.0f / 441.0f;
                if (d->drop_xf <= 0) { d->drop_xf = 0; d->drop_lag = 0; }
            }
            l = l * (1 - d->drop_xf) + d->drop_lp[0] * d->drop_xf;
            r = r * (1 - d->drop_xf) + d->drop_lp[1] * d->drop_xf;
        } else { d->drop_lp[0] = l; d->drop_lp[1] = r; }

        /* ---- CRUSH: bit depth + sample-rate reduction ---- */
        float a_cr = p[P_CRUSH_ON] > 0.5f ? f * p[P_CRUSH] / 100.0f : 0.0f;
        if (a_cr > 0.002f) {
            float hold = 1.0f + a_cr * a_cr * 40.0f;
            d->crush_cnt += 1.0f;
            if (d->crush_cnt >= hold) { d->crush_cnt -= hold; d->crush_hold[0] = l; d->crush_hold[1] = r; }
            float steps = powf(2.0f, 15.0f - a_cr * 12.0f);
            float cl = floorf(d->crush_hold[0] * steps + 0.5f) / steps, crr = floorf(d->crush_hold[1] * steps + 0.5f) / steps;
            float m = clampf(a_cr * 4.0f, 0, 1);                   /* fades in over the first quarter */
            l = l * (1 - m) + cl * m;
            r = r * (1 - m) + crr * m;
        } else { d->crush_cnt = 0; d->crush_hold[0] = l; d->crush_hold[1] = r; }

        /* ---- FILTER: bipolar DJ filter (Amount < 0 low-pass, > 0 high-pass) ---- */
        float a_f = p[P_FLT_ON] > 0.5f ? f * p[P_FLT] / 100.0f : 0.0f;
        d->flt_amt_s += (a_f - d->flt_amt_s) * 0.002f;
        d->flt_res_s += (p[P_FLT_RES] / 100.0f - d->flt_res_s) * 0.002f;
        {
            float amt = d->flt_amt_s, fc;
            int mode;
            if (amt < 0) { mode = 0; fc = 20000.0f * powf(90.0f / 20000.0f, -amt); }
            else         { mode = 1; fc = 20.0f * powf(9000.0f / 20.0f, amt); }
            float g = tanf(3.14159265f * clampf(fc, 20.0f, 20000.0f) / SR);
            float res = d->flt_res_s * clampf(fabsf(amt) * 3.0f, 0, 1);   /* no resonance bump at rest */
            float k = 2.0f - 1.85f * res;
            float fl = svf(&d->flt[0], l, g, k, mode), fr = svf(&d->flt[1], r, g, k, mode);
            /* at rest the filter is crossfaded out: even a 20 Hz high-pass shifts the phase of a
             * kick enough to be heard as a change, and "fader at 0" must mean untouched */
            float m = clampf(fabsf(amt) * 20.0f, 0, 1);
            l = l * (1 - m) + fl * m;
            r = r * (1 - m) + fr * m;
        }

        /* ---- GATER: tempo-synced chop, restarts on the beat it was engaged ---- */
        float a_g = p[P_GATE_ON] > 0.5f ? f * p[P_GATE] / 100.0f : 0.0f;
        if (a_g > 0.005f) {
            float period = qn * frac_of_div((int)p[P_GATE_DIV], GATE_FRAC, 4);
            d->gate_phase += 1.0 / period;
            if (d->gate_phase >= 1.0) d->gate_phase -= 1.0;
            float target = d->gate_phase < 0.5 ? 1.0f : 1.0f - clampf(a_g * 1.25f, 0, 1);
            d->gate_g += (target - d->gate_g) * 0.02f;             /* ~1 ms edges: no clicks */
        } else { d->gate_phase = 0; d->gate_g += (1.0f - d->gate_g) * 0.02f; }
        l *= d->gate_g; r *= d->gate_g;

        /* ---- RISER: band-passed noise sweeping up with the fader ---- */
        float a_ri = p[P_RISE_ON] > 0.5f ? f : 0.0f;
        if (a_ri > 0.01f) {
            float lvl = p[P_RISE] / 100.0f * a_ri * a_ri * 0.35f;
            float g = tanf(3.14159265f * (250.0f * powf(48.0f, a_ri)) / SR);
            l += svf(&d->rise_bp[0], noise(d), g, 0.7f, 2) * lvl;
            r += svf(&d->rise_bp[1], noise(d), g, 0.7f, 2) * lvl;
        }

        /* ---- DELAY throw: send follows the fader, the tail doesn't ---- */
        float send_d = p[P_DLY_ON] > 0.5f ? f * p[P_DLY] / 100.0f : 0.0f;
        float dt = qn * frac_of_div((int)p[P_DLY_DIV], DLY_FRAC, 6);
        if (dt > DLY_LEN - 8) dt = DLY_LEN - 8;
        if (d->dly_time_s <= 0) d->dly_time_s = dt;
        d->dly_time_s += (dt - d->dly_time_s) * 0.0005f;
        float fb = clampf(p[P_DLY_FB] / 100.0f, 0, 0.9f);
        double dpos = (double)d->dpos - d->dly_time_s;
        float wl = rd(d->dly[0], DLY_LEN - 1, dpos), wr = rd(d->dly[1], DLY_LEN - 1, dpos);
        d->dly_lp[0] += (wl - d->dly_lp[0]) * 0.35f;              /* darker repeats */
        d->dly_lp[1] += (wr - d->dly_lp[1]) * 0.35f;
        d->dly[0][d->dpos & (DLY_LEN - 1)] = l * send_d + d->dly_lp[1] * fb;   /* cross-feedback: ping-pong */
        d->dly[1][d->dpos & (DLY_LEN - 1)] = r * send_d + d->dly_lp[0] * fb;
        d->dpos++;

        /* ---- REVERB: 4-line FDN; FREEZE holds the tail at full fader ---- */
        float send_r = p[P_REV_ON] > 0.5f ? f * p[P_REV] / 100.0f : 0.0f;
        int frozen = p[P_REV_ON] > 0.5f && p[P_REV_FREEZE] > 0.5f && f > 0.95f;
        d->rev_size_s += (p[P_REV_SIZE] / 100.0f - d->rev_size_s) * 0.0002f;
        float size = 0.45f + d->rev_size_s * 0.55f;
        float gfb = frozen ? 1.0f : 0.70f + d->rev_size_s * 0.27f;
        float o[FDN_N], s = 0;
        for (int i = 0; i < FDN_N; i++) {
            int len = (int)(FDN_BASE[i] * size);
            o[i] = d->fdn[i][(d->fdn_pos[i] - len + FDN_MAX) & (FDN_MAX - 1)];
            if (!frozen) { d->fdn_lp[i] += (o[i] - d->fdn_lp[i]) * 0.45f; o[i] = d->fdn_lp[i]; }
            s += o[i];
        }
        float h = s * 0.5f;                                        /* Householder mix: lossless */
        float rin = frozen ? 0.0f : (l + r) * 0.5f * send_r * 0.6f;
        for (int i = 0; i < FDN_N; i++) {
            d->fdn[i][d->fdn_pos[i]] = (o[i] - h) * gfb + rin;
            d->fdn_pos[i] = (d->fdn_pos[i] + 1) & (FDN_MAX - 1);
        }
        float rvl = (o[0] + o[2]) * 0.5f, rvr = (o[1] + o[3]) * 0.5f;

        l += wl + rvl;
        r += wr + rvr;
        out[2 * n]     = (int16_t)lrintf(clampf(l, -1.0f, 32767.0f / 32768.0f) * 32768.0f);
        out[2 * n + 1] = (int16_t)lrintf(clampf(r, -1.0f, 32767.0f / 32768.0f) * 32768.0f);
    }
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, process };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
