/* Offline x86 test of Fader FX (engine + mpc-vst-plugins' generic wrapper, ASan/UBSan):
 * real audio in (a kick + bass-ish test signal), every effect alone at full fader,
 * fader 0 transparent, delay/reverb tails outliving the fader, freeze sustaining,
 * chunk round-trip, rough CPU cost per block. Prints PASSED/FAILED. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct AEffect AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*d)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setP)(AEffect *, int32_t, float);
    float (*getP)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*pr)(AEffect *, float **, float **, int32_t);
    void (*prd)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
AEffect *VSTPluginMain(audioMasterCallback);

static VstTimeInfo ti = {.tempo = 124.0, .sampleRate = 44100, .flags = 1 << 10};
static intptr_t master(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) {
    (void)e; (void)i; (void)v; (void)p; (void)o;
    return op == 7 ? (intptr_t)&ti : op == 1 ? 2400 : 0;
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static long t_samples;
static float sig(int ch) {          /* 124 BPM kick + a detuned saw bass: something to chop */
    double t = t_samples / 44100.0, beat = fmod(t, 60.0 / 124.0);
    double kick = sin(2 * M_PI * (50 + 120 * exp(-beat * 30)) * beat) * exp(-beat * 9) * 0.5;
    double f = ch ? 55.3 : 55.0, saw = 2 * fmod(t * f, 1.0) - 1;
    return (float)(kick + saw * 0.15);
}
/* run n blocks; returns rms of output, and max |out - in| (diff) */
static double run(AEffect *a, int blocks, double *maxdiff, double *peak) {
    float il[128], ir[128], ol[128], orr[128], *in[2] = {il, ir}, *out[2] = {ol, orr};
    double e = 0, md = 0, pk = 0;
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 128; i++) { il[i] = sig(0); ir[i] = sig(1); t_samples++; }
        a->pr(a, in, out, 128);
        for (int i = 0; i < 128; i++) {
            CHECK(isfinite(ol[i]) && isfinite(orr[i]), "non-finite output");
            if (!isfinite(ol[i])) return -1;
            e += ol[i] * ol[i] + orr[i] * orr[i];
            double d = fabs(ol[i] - il[i]); if (d > md) md = d;
            if (fabs(ol[i]) > pk) pk = fabs(ol[i]);
        }
    }
    if (maxdiff) *maxdiff = md;
    if (peak) *peak = pk;
    return sqrt(e / (blocks * 256.0));
}
static int idx(AEffect *a, const char *name) {
    char b[64];
    for (int i = 0; i < a->numParams; i++) { a->d(a, 8, i, 0, b, 0); if (!strcmp(b, name)) return i; }
    printf("no param %s\n", name); fails++; return 0;
}
static const char *ONS[] = {"Filter", "Repeat", "Crush", "Gater", "Riser", "Drop", "Delay", "Reverb"};

int main(void) {
    AEffect *a = VSTPluginMain(master);
    CHECK(a && a->magic == 0x56737450 && a->numInputs == 2, "effect instance");
    int fader = idx(a, "Fader");
    double md, pk, rms;

    for (int k = 0; k < 8; k++) a->setP(a, idx(a, ONS[k]), 0.0f);
    run(a, 200, 0, 0);
    rms = run(a, 100, &md, 0);
    printf("all off, fader 0: max diff %.5f\n", md);
    CHECK(md < 0.002, "all effects off must be (nearly) transparent: %.5f", md);

    for (int k = 0; k < 8; k++) a->setP(a, idx(a, ONS[k]), 1.0f);
    a->setP(a, fader, 0.0f);
    run(a, 200, 0, 0);
    run(a, 800, 0, 0);                                    /* let delay/reverb tails die (fader 0: no send) */
    rms = run(a, 100, &md, 0);
    printf("all on, fader 0: max diff %.5f\n", md);
    CHECK(md < 0.01, "fader 0 must leave the signal (nearly) untouched: %.5f", md);

    for (int k = 0; k < 8; k++) {                          /* each effect alone, full fader */
        for (int j = 0; j < 8; j++) a->setP(a, idx(a, ONS[j]), j == k ? 1.0f : 0.0f);
        a->setP(a, fader, 1.0f);
        run(a, 60, 0, 0);
        rms = run(a, 300, &md, &pk);
        printf("%-7s full: rms %.3f, max diff %.3f, peak %.3f\n", ONS[k], rms, md, pk);
        CHECK(md > 0.02, "%s audibly changes the signal", ONS[k]);
        CHECK(rms > 0.005 || k == 3, "%s not silent", ONS[k]);
        a->setP(a, fader, 0.0f);
        run(a, 600, 0, 0);
    }

    /* delay throw: tail outlives the fader */
    for (int j = 0; j < 8; j++) a->setP(a, idx(a, ONS[j]), j == 6 ? 1.0f : 0.0f);
    a->setP(a, fader, 1.0f); run(a, 150, 0, 0);
    a->setP(a, fader, 0.0f); run(a, 20, 0, 0);
    run(a, 60, &md, 0);
    printf("delay tail after fader back: max diff %.3f\n", md);
    CHECK(md > 0.01, "delay tail keeps ringing after the fader is back");

    /* reverb freeze: sustains at full fader */
    for (int j = 0; j < 8; j++) a->setP(a, idx(a, ONS[j]), j == 7 ? 1.0f : 0.0f);
    a->setP(a, idx(a, "Freeze"), 1.0f);
    a->setP(a, fader, 0.9f); run(a, 300, 0, 0);
    a->setP(a, fader, 1.0f); run(a, 100, 0, 0);
    double r1 = run(a, 200, 0, 0), r2 = run(a, 800, 0, 0);
    printf("freeze: rms %.3f then %.3f\n", r1, r2);
    CHECK(r2 > r1 * 0.7, "frozen tail sustains");
    a->setP(a, idx(a, "Freeze"), 0.0f);
    a->setP(a, fader, 0.0f); run(a, 400, 0, 0);

    /* chunk round-trip */
    int flt = idx(a, "Filter LP/HP");
    a->setP(a, flt, 0.1f);
    void *ck = 0; intptr_t n = a->d(a, 23, 0, 0, &ck, 0);
    char saved[2048]; memcpy(saved, ck, n);
    a->setP(a, flt, 0.9f);
    a->d(a, 24, 0, n, saved, 0);
    CHECK(fabsf(a->getP(a, flt) - 0.1f) < 0.01f, "chunk restores Filter LP/HP (%.3f)", a->getP(a, flt));

    /* rough cost: everything on at full fader */
    for (int j = 0; j < 8; j++) a->setP(a, idx(a, ONS[j]), 1.0f);
    a->setP(a, fader, 1.0f);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    run(a, 2000, 0, &pk);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / 2000;
    printf("all 8 at full: %.1f us per 128-frame block on this x86 (sanitizers on), peak %.2f\n", us, pk);

    a->d(a, 1, 0, 0, 0, 0);
    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails != 0;
}
