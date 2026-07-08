/* tests/arm64/fdn_test.c - host unit tests for the FDN reverb (Theme M20,
 * issue #191).
 *
 * What a product-grade reverb must do, measured on the impulse response:
 *   - a dense tail with no gaps (every window carries energy), decaying
 *     monotonically;
 *   - the decay time TRACKS the rt60 control (measured by regressing log
 *     energy over time and comparing against the setting);
 *   - damping makes highs die faster than lows;
 *   - the output stays bounded (and finite) across the full control range,
 *     with and without delay modulation;
 *   - mix 0 is bit-exactly dry.
 *
 * Build/run via:  make test-arm-fdn
 */

#include "tessera.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define SR 48000.0f

/* Mutually-prime line lengths in the 21..44 ms range at 48 kHz. */
static const uint32_t g_sizes[TESSERA_FDN_LINES] = {
    1031, 1201, 1327, 1459, 1597, 1747, 1889, 2111
};
static float g_lines[TESSERA_FDN_LINES][2111];

static void make(tessera_fx_reverb2_t *r, float rt60, float size,
                 float damp, float mix)
{
    float *bufs[TESSERA_FDN_LINES];
    for (int i = 0; i < TESSERA_FDN_LINES; i++)
        bufs[i] = g_lines[i];
    tessera_fx_reverb2_init(r, SR, bufs, g_sizes, rt60, size, damp, mix);
}

/* Render `n` samples of impulse response (wet only). */
static void impulse(tessera_fx_reverb2_t *r, float *out, uint32_t n)
{
    out[0] = tessera_fx_reverb2(r, 1.0f);
    for (uint32_t i = 1; i < n; i++)
        out[i] = tessera_fx_reverb2(r, 0.0f);
}

static double win_energy(const float *x, uint32_t start, uint32_t len)
{
    double e = 0.0;
    for (uint32_t i = start; i < start + len; i++)
        e += (double)x[i] * x[i];
    return e;
}

/* Measured RT60: regress log10(window energy) over the window centres; the
 * energy slope is -60/rt60 dB/s (energy in dB falls 60/rt60 per second... and
 * squared amplitude falls at exactly the amplitude dB rate x2 / 2 - i.e.
 * 10*log10(E) tracks the level in dB). */
static double measure_rt60(const float *x, uint32_t n)
{
    enum { W = 4800 };                     /* 100 ms windows */
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int m = 0;
    for (uint32_t s = 4800; s + W <= n; s += W) {   /* skip the build-up */
        double e = win_energy(x, s, W);
        if (e <= 0.0) break;
        double t  = ((double)s + W / 2.0) / (double)SR;
        double db = 10.0 * log10(e);
        sx += t; sy += db; sxx += t * t; sxy += t * db;
        m++;
    }
    if (m < 3) return 0.0;
    double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);   /* dB/s */
    return slope < 0.0 ? -60.0 / slope : 0.0;
}

/* ---- density and monotone decay ------------------------------------------- */

static void test_tail(void)
{
    printf("- impulse: dense, gap-free, monotonically decaying tail\n");
    tessera_fx_reverb2_t r;
    make(&r, 1.0f, 1.0f, 0.2f, 1.0f);

    enum { N = 96000 };                    /* 2 s */
    static float ir[N];
    impulse(&r, ir, N);

    /* No gaps: every 10 ms window in [100 ms, 1 s] carries energy. */
    int gaps = 0;
    for (uint32_t s = 4800; s < 48000; s += 480)
        if (win_energy(ir, s, 480) < 1e-12)
            gaps++;
    CHECK(gaps == 0, "no silent 10 ms window in the first second of tail");

    /* Monotone decay of 100 ms energies after the build-up. */
    int monotone = 1;
    double prev = win_energy(ir, 4800, 4800);
    for (uint32_t s = 9600; s + 4800 <= N; s += 4800) {
        double e = win_energy(ir, s, 4800);
        if (e > prev * 1.05) monotone = 0;     /* 5% ripple allowance */
        prev = e;
    }
    CHECK(monotone, "window energy decays monotonically");

    /* Density: in [100, 200] ms, most samples are non-negligible relative to
     * the window RMS - a sparse comb pattern would fail this. */
    double e = win_energy(ir, 4800, 4800);
    double rms = sqrt(e / 4800.0);
    int busy = 0;
    for (uint32_t i = 4800; i < 9600; i++)
        if (fabsf(ir[i]) > 0.05f * rms) busy++;
    printf("    %d/4800 samples above 5%% of window RMS\n", busy);
    CHECK(busy > 3000, "tail is dense (echo density from the Hadamard mixing)");
}

static void test_rt60_tracks(void)
{
    printf("- the decay time tracks the rt60 control\n");
    enum { N = 144000 };                   /* 3 s */
    static float ir[N];

    tessera_fx_reverb2_t r;
    make(&r, 0.5f, 1.0f, 0.0f, 1.0f);
    impulse(&r, ir, N);
    double m05 = measure_rt60(ir, 48000);
    printf("    rt60=0.5 s -> measured %.2f s\n", m05);
    CHECK(m05 > 0.35 && m05 < 0.65, "0.5 s setting measures ~0.5 s");

    make(&r, 2.0f, 1.0f, 0.0f, 1.0f);
    impulse(&r, ir, N);
    double m20 = measure_rt60(ir, N);
    printf("    rt60=2.0 s -> measured %.2f s\n", m20);
    CHECK(m20 > 1.4 && m20 < 2.6, "2.0 s setting measures ~2.0 s");
    CHECK(m20 > 2.5 * m05, "longer setting, correspondingly longer tail");
}

static void test_damping(void)
{
    printf("- damping: highs die faster than lows\n");
    enum { N = 48000 };
    static float bright[N], dark[N];

    tessera_fx_reverb2_t r;
    make(&r, 1.5f, 1.0f, 0.0f, 1.0f);
    impulse(&r, bright, N);
    make(&r, 1.5f, 1.0f, 0.7f, 1.0f);
    impulse(&r, dark, N);

    /* Compare high-band energy (one-pole HP proxy: differences) late in the
     * tail, normalised by total energy so the overall decay cancels out. */
    double hb = 0, hd = 0, tb = 0, td = 0;
    for (uint32_t i = 24001; i < N; i++) {
        double db_ = bright[i] - bright[i - 1], dd = dark[i] - dark[i - 1];
        hb += db_ * db_; hd += dd * dd;
        tb += (double)bright[i] * bright[i];
        td += (double)dark[i] * dark[i];
    }
    double ratio_bright = hb / (tb + 1e-30), ratio_dark = hd / (td + 1e-30);
    printf("    HF fraction: damp=0 %.3f, damp=0.7 %.3f\n",
           ratio_bright, ratio_dark);
    CHECK(ratio_dark < 0.5 * ratio_bright,
          "damped tail carries proportionally far less HF");
}

static void test_stability(void)
{
    printf("- bounded output across the full control range (incl. modulation)\n");
    static float noise[24000];
    uint32_t seed = 0xfdb97531u;
    for (uint32_t i = 0; i < 24000; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        noise[i] = (float)(int32_t)seed / 2147483648.0f * 0.5f;
    }

    const float rt60s[] = { 0.2f, 1.0f, 5.0f, 10.0f };
    const float damps[] = { 0.0f, 0.5f, 0.9f };
    int ok = 1;
    for (int a = 0; a < 4 && ok; a++)
        for (int b = 0; b < 3 && ok; b++)
            for (int mod = 0; mod <= 1 && ok; mod++) {
                tessera_fx_reverb2_t r;
                make(&r, rt60s[a], 1.0f, damps[b], 0.5f);
                if (mod)
                    tessera_fx_reverb2_mod(&r, 0.7f, 6.0f);
                float peak = 0.0f;
                for (uint32_t i = 0; i < 24000; i++) {
                    float y = tessera_fx_reverb2(&r, noise[i]);
                    float m = y < 0.0f ? -y : y;
                    if (m > peak) peak = m;
                    if (!(m < 1e6f)) { ok = 0; break; }   /* NaN/inf guard */
                }
                for (uint32_t i = 0; i < 48000; i++) {    /* ring out */
                    float y = tessera_fx_reverb2(&r, 0.0f);
                    float m = y < 0.0f ? -y : y;
                    if (m > peak) peak = m;
                    if (!(m < 1e6f)) { ok = 0; break; }
                }
                if (peak > 4.0f) ok = 0;
            }
    CHECK(ok, "every {rt60, damp, mod} combination stays bounded and finite");
}

static void test_modulated_tail_decays(void)
{
    printf("- modulated tail still decays to silence\n");
    tessera_fx_reverb2_t r;
    make(&r, 0.5f, 1.0f, 0.3f, 1.0f);
    tessera_fx_reverb2_mod(&r, 1.1f, 5.0f);
    enum { N = 144000 };
    static float ir[N];
    impulse(&r, ir, N);
    double late = win_energy(ir, N - 4800, 4800);
    double early = win_energy(ir, 4800, 4800);
    CHECK(late < 1e-6 * early, "3 s after the impulse the tail is ~gone");
}

static void test_dry_mix(void)
{
    printf("- mix 0 is exactly dry\n");
    tessera_fx_reverb2_t r;
    make(&r, 2.0f, 1.0f, 0.3f, 0.0f);
    int exact = 1;
    for (int i = 0; i < 4800; i++) {
        float x = (float)i * 0.0001f - 0.2f;
        if (tessera_fx_reverb2(&r, x) != x) exact = 0;
    }
    CHECK(exact, "mix=0 passes the input through bit-exactly");
}

int main(void)
{
    printf("=== FDN reverb host tests (issue #191) ===\n");
    test_tail();
    test_rt60_tracks();
    test_damping();
    test_stability();
    test_modulated_tail_decays();
    test_dry_mix();

    if (g_fail) {
        printf("FDN TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("FDN TESTS: ALL PASS\n");
    return 0;
}
