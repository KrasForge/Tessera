/* tests/arm64/vocoder_test.c - host unit tests for the phase vocoder (Theme
 * M18, issue #186).
 *
 * What must hold:
 *   - unity settings (stretch 1.0) reproduce the input within a small
 *     tolerance, minus the framework latency (found by cross-correlation);
 *   - a 2x time-stretch produces twice the output while PRESERVING the
 *     tone's frequency (that is what distinguishes a vocoder from a
 *     resampler);
 *   - a +12-semitone pitch shift moves a sine's measured fundamental up one
 *     octave (Goertzel probes), with the output bounded and the artifact
 *     energy limited;
 *   - the pitch shifter preserves duration (same in/out count per call).
 *
 * Build/run via:  make test-arm-vocoder
 */

#include "tessera.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define N     1024u
#define SR    48000.0
#define BINS  (N / 2u + 1u)

static tessera_cpx_t g_tw[N / 2u];
static float         g_mem[4u * N + 4u * BINS + 6u * N];   /* pshift arena */
static tessera_cpx_t g_cmem[BINS];

/* Goertzel power of `x` at frequency f. */
static double goertzel(const float *x, uint32_t n, double f)
{
    double w = 2.0 * M_PI * f / SR;
    double c = 2.0 * cos(w), s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        s0 = (double)x[i] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

static double rms(const float *x, uint32_t n)
{
    double s = 0.0;
    for (uint32_t i = 0; i < n; i++) s += (double)x[i] * x[i];
    return sqrt(s / (double)n);
}

/* ---- unity stretch reproduces the input ---------------------------------- */

static void test_unity(void)
{
    printf("- unity ratio reproduces the input (minus framework latency)\n");
    tessera_pvoc_t pv;
    CHECK(tessera_pvoc_init(&pv, N, 1.0f, g_tw, g_mem, g_cmem) == 0, "init ok");
    CHECK(pv.ha == pv.hs, "unity: analysis hop == synthesis hop");

    enum { HOPS = 64 };
    static float in[HOPS * (N / 4u)], out[HOPS * (N / 4u)];
    uint32_t total = HOPS * pv.ha;
    for (uint32_t i = 0; i < total; i++)   /* two incommensurate tones */
        in[i] = (float)(0.5 * sin(2.0 * M_PI * 441.3 * i / SR) +
                        0.3 * sin(2.0 * M_PI * 1327.9 * i / SR));
    for (uint32_t h = 0; h < HOPS; h++)
        tessera_pvoc_process(&pv, in + h * pv.ha, out + h * pv.hs);

    /* Find the latency by cross-correlation over a plausible range. */
    uint32_t best = 0; double bestc = -1e30;
    for (uint32_t lag = 0; lag <= 2u * N; lag += 1u) {
        double c = 0.0;
        for (uint32_t i = 0; i + lag < total && i < 4096u; i++)
            c += (double)out[i + lag] * in[i];
        if (c > bestc) { bestc = c; best = lag; }
    }
    printf("    latency = %u samples\n", best);

    /* Steady-state error, skipping the priming region. */
    double err = 0.0, ref = 0.0;
    uint32_t start = 4u * N;
    for (uint32_t i = start; i + best < total; i++) {
        double d = (double)out[i + best] - in[i];
        err += d * d;
        ref += (double)in[i] * in[i];
    }
    double rel = sqrt(err / ref);
    printf("    steady-state relative RMS error = %.3f\n", rel);
    CHECK(rel < 0.05, "unity output matches the input within 5% RMS");
}

/* ---- 2x time-stretch preserves pitch -------------------------------------- */

static void test_stretch(void)
{
    printf("- 2x time-stretch: twice the samples, same pitch\n");
    tessera_pvoc_t pv;
    CHECK(tessera_pvoc_init(&pv, N, 2.0f, g_tw, g_mem, g_cmem) == 0, "init ok");
    CHECK(pv.hs == 2u * pv.ha, "ratio 2 = consume ha, produce 2*ha");

    /* 750 Hz = an exact bin at N=1024, SR=48k (bin 16), minimising leakage. */
    enum { HOPS = 96 };
    static float in[HOPS * 256u], out[HOPS * 256u];
    uint32_t total_in = HOPS * pv.ha, total_out = HOPS * pv.hs;
    for (uint32_t i = 0; i < total_in; i++)
        in[i] = (float)sin(2.0 * M_PI * 750.0 * i / SR);
    for (uint32_t h = 0; h < HOPS; h++)
        tessera_pvoc_process(&pv, in + h * pv.ha, out + h * pv.hs);

    /* Probe the steady-state tail of the stretched output. */
    const float *tail = out + total_out / 2u;
    uint32_t     tn   = total_out / 2u;
    double p750  = goertzel(tail, tn, 750.0);
    double p375  = goertzel(tail, tn, 375.0);
    double p1500 = goertzel(tail, tn, 1500.0);
    CHECK(p750 > 100.0 * p375 && p750 > 100.0 * p1500,
          "stretched tone stays at 750 Hz (not resampled down or up)");
    CHECK(rms(tail, tn) > 0.5, "stretched tone keeps its level");
}

/* ---- +12 semitone pitch shift --------------------------------------------- */

static void test_pitch_up_octave(void)
{
    printf("- pitch shift +12 st: 440 Hz measures as 880 Hz\n");
    tessera_pshift_t ps;
    CHECK(tessera_pshift_init(&ps, N, 2.0f, g_tw, g_mem, g_cmem) == 0, "init ok");

    enum { BLK = 256, NBLK = 220 };
    static float in[BLK], out[NBLK * BLK];
    uint32_t t = 0;
    for (uint32_t b = 0; b < NBLK; b++) {
        for (uint32_t i = 0; i < BLK; i++, t++)
            in[i] = (float)sin(2.0 * M_PI * 440.0 * t / SR);
        tessera_pshift_process(&ps, in, out + b * BLK, BLK);
    }

    /* Steady-state region (skip priming + resampler warm-up). */
    const float *ss = out + (NBLK * BLK) / 2u;
    uint32_t     sn = (NBLK * BLK) / 2u;

    double p880 = goertzel(ss, sn, 880.0);
    double p440 = goertzel(ss, sn, 440.0);
    double p1760 = goertzel(ss, sn, 1760.0);
    printf("    880/440 power ratio = %.1f\n", p880 / (p440 + 1e-9));
    CHECK(p880 > 50.0 * p440, "fundamental moved up an octave");
    CHECK(p880 > 50.0 * p1760, "no dominant second harmonic artifact");

    /* Bounded output and limited artifact energy: the 880 Hz tone carries
     * most of the signal power. */
    float peak = 0.0f;
    for (uint32_t i = 0; i < sn; i++) {
        float a = ss[i] < 0.0f ? -ss[i] : ss[i];
        if (a > peak) peak = a;
    }
    CHECK(peak < 1.5f, "output stays bounded");

    /* Compare tone power against total power: Goertzel power of a sine of
     * amplitude A over n samples is ~ (A*n/2)^2. */
    double a880 = 2.0 * sqrt(p880) / (double)sn;         /* est. amplitude */
    double ptotal = 0.0;
    for (uint32_t i = 0; i < sn; i++) ptotal += (double)ss[i] * ss[i];
    double ptone = a880 * a880 / 2.0 * (double)sn;
    printf("    tone fraction of total power = %.2f\n", ptone / ptotal);
    CHECK(ptone / ptotal > 0.8, "artifact energy limited (tone > 80% of power)");
}

static void test_pitch_down(void)
{
    printf("- pitch shift -12 st: 880 Hz measures as 440 Hz\n");
    tessera_pshift_t ps;
    CHECK(tessera_pshift_init(&ps, N, 0.5f, g_tw, g_mem, g_cmem) == 0, "init ok");

    enum { BLK = 256, NBLK = 220 };
    static float in[BLK], out[NBLK * BLK];
    uint32_t t = 0;
    for (uint32_t b = 0; b < NBLK; b++) {
        for (uint32_t i = 0; i < BLK; i++, t++)
            in[i] = (float)sin(2.0 * M_PI * 880.0 * t / SR);
        tessera_pshift_process(&ps, in, out + b * BLK, BLK);
    }
    const float *ss = out + (NBLK * BLK) / 2u;
    uint32_t     sn = (NBLK * BLK) / 2u;
    double p440 = goertzel(ss, sn, 440.0);
    double p880 = goertzel(ss, sn, 880.0);
    CHECK(p440 > 50.0 * p880, "fundamental moved down an octave");
}

static void test_duration_preserved(void)
{
    printf("- the pitch shifter preserves duration exactly\n");
    /* Same in/out count per call is structural (the API contract): verify a
     * long run keeps producing real (non-silent) output at unity level. */
    tessera_pshift_t ps;
    tessera_pshift_init(&ps, N, 1.2599f, g_tw, g_mem, g_cmem);   /* +4 st */
    enum { BLK = 192, NBLK = 200 };                /* block !~ hop: stress FIFOs */
    static float in[BLK], out[BLK];
    uint32_t t = 0;
    double tail_rms = 0.0;
    for (uint32_t b = 0; b < NBLK; b++) {
        for (uint32_t i = 0; i < BLK; i++, t++)
            in[i] = (float)sin(2.0 * M_PI * 500.0 * t / SR);
        tessera_pshift_process(&ps, in, out, BLK);
        if (b >= NBLK - 50u)
            tail_rms += rms(out, BLK);
    }
    tail_rms /= 50.0;
    printf("    steady-state block RMS = %.3f\n", tail_rms);
    CHECK(tail_rms > 0.5 && tail_rms < 0.9,
          "steady output at roughly the input level (no drops, no pile-up)");
}

static void test_guards(void)
{
    printf("- guards\n");
    tessera_pvoc_t pv;
    CHECK(tessera_pvoc_init(&pv, 100u, 1.0f, g_tw, g_mem, g_cmem) == -1,
          "non-power-of-two size refused");
    CHECK(tessera_pvoc_init(&pv, N, 0.1f, g_tw, g_mem, g_cmem) == -1,
          "ratio out of range refused");
    CHECK(tessera_pvoc_floats(N) == 4u * N + 4u * BINS &&
          tessera_pshift_floats(N) == tessera_pvoc_floats(N) + 6u * N,
          "arena sizing helpers");
}

int main(void)
{
    printf("=== phase vocoder host tests (issue #186) ===\n");
    tessera_fft_twiddles(g_tw, N);

    test_unity();
    test_stretch();
    test_pitch_up_octave();
    test_pitch_down();
    test_duration_preserved();
    test_guards();

    if (g_fail) {
        printf("VOCODER TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("VOCODER TESTS: ALL PASS\n");
    return 0;
}
