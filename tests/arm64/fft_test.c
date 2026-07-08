/* tests/arm64/fft_test.c - host unit tests for the SDK FFT primitive (Theme
 * M18, issue #184).
 *
 * The transforms are checked against a naive O(n^2) DFT reference built on
 * libm (the SDK code itself uses none): forward matches the reference, the
 * inverse round-trips, the packed real pair matches the full complex result
 * bin for bin, a pure tone concentrates in the right bin and a two-tone
 * signal in two, and the periodic windows satisfy the overlap-add identities
 * STFT resynthesis relies on.
 *
 * Build/run via:  make test-arm-fft
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

/* Deterministic PRNG (no rand()): xorshift32, mapped to [-1, 1). */
static uint32_t g_seed = 0x1234567u;
static float frand(void)
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return (float)(int32_t)g_seed / 2147483648.0f;
}

/* Naive DFT reference (libm). */
static void dft_ref(const tessera_cpx_t *in, tessera_cpx_t *out, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        double re = 0.0, im = 0.0;
        for (uint32_t j = 0; j < n; j++) {
            double a = -2.0 * M_PI * (double)j * (double)k / (double)n;
            re += in[j].re * cos(a) - in[j].im * sin(a);
            im += in[j].re * sin(a) + in[j].im * cos(a);
        }
        out[k].re = (float)re;
        out[k].im = (float)im;
    }
}

static float cpx_maxerr(const tessera_cpx_t *a, const tessera_cpx_t *b, uint32_t n)
{
    float m = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float dre = fabsf(a[i].re - b[i].re);
        float dim = fabsf(a[i].im - b[i].im);
        if (dre > m) m = dre;
        if (dim > m) m = dim;
    }
    return m;
}

#define N 256u

static tessera_cpx_t g_tw[N / 2u];

static void test_complex_vs_dft(void)
{
    printf("- complex FFT vs a naive DFT reference (n=%u)\n", N);
    tessera_cpx_t x[N], want[N];
    for (uint32_t i = 0; i < N; i++) { x[i].re = frand(); x[i].im = frand(); }
    dft_ref(x, want, N);
    tessera_fft(x, g_tw, N);
    float err = cpx_maxerr(x, want, N);
    printf("    max |fft - dft| = %.2e\n", (double)err);
    CHECK(err < 1e-3f, "forward FFT matches the DFT reference");
}

static void test_roundtrip(void)
{
    printf("- inverse round-trip\n");
    tessera_cpx_t x[N], orig[N];
    for (uint32_t i = 0; i < N; i++) { x[i].re = frand(); x[i].im = frand(); }
    memcpy(orig, x, sizeof x);
    tessera_fft(x, g_tw, N);
    tessera_ifft(x, g_tw, N);
    float err = cpx_maxerr(x, orig, N);
    printf("    max round-trip error = %.2e\n", (double)err);
    CHECK(err < 1e-5f, "ifft(fft(x)) == x within 1e-5");
}

static void test_rfft_vs_complex(void)
{
    printf("- packed real FFT matches the full complex transform\n");
    float xr[N];
    tessera_cpx_t xc[N], bins[N / 2u + 1u];
    for (uint32_t i = 0; i < N; i++) {
        xr[i] = frand();
        xc[i].re = xr[i]; xc[i].im = 0.0f;
    }
    tessera_fft(xc, g_tw, N);
    tessera_rfft(xr, bins, g_tw, N);

    float m = 0.0f;
    for (uint32_t k = 0; k <= N / 2u; k++) {
        float dre = fabsf(bins[k].re - xc[k].re);
        float dim = fabsf(bins[k].im - xc[k].im);
        if (dre > m) m = dre;
        if (dim > m) m = dim;
    }
    printf("    max |rfft - fft| over bins 0..n/2 = %.2e\n", (double)m);
    CHECK(m < 1e-3f, "rfft bins equal the complex FFT's");
    CHECK(bins[0].im == 0.0f && bins[N / 2u].im == 0.0f,
          "DC and Nyquist bins are purely real");
}

static void test_rfft_roundtrip(void)
{
    printf("- real inverse round-trip\n");
    float x[N], orig[N];
    tessera_cpx_t bins[N / 2u + 1u];
    for (uint32_t i = 0; i < N; i++) { x[i] = frand(); orig[i] = x[i]; }
    tessera_rfft(x, bins, g_tw, N);
    tessera_irfft(bins, x, g_tw, N);
    float m = 0.0f;
    for (uint32_t i = 0; i < N; i++) {
        float d = fabsf(x[i] - orig[i]);
        if (d > m) m = d;
    }
    printf("    max round-trip error = %.2e\n", (double)m);
    CHECK(m < 1e-5f, "irfft(rfft(x)) == x within 1e-5");
}

static void test_tones(void)
{
    printf("- pure and two-tone signals concentrate in the right bins\n");
    float x[N];
    tessera_cpx_t bins[N / 2u + 1u];

    /* Pure tone at bin 13 (integer-periodic, so no leakage). */
    for (uint32_t i = 0; i < N; i++)
        x[i] = (float)cos(2.0 * M_PI * 13.0 * (double)i / (double)N);
    tessera_rfft(x, bins, g_tw, N);

    uint32_t peak = 0; double pmag = 0.0, rest = 0.0;
    for (uint32_t k = 0; k <= N / 2u; k++) {
        double mag = sqrt((double)bins[k].re * bins[k].re +
                          (double)bins[k].im * bins[k].im);
        if (mag > pmag) { pmag = mag; peak = k; }
    }
    for (uint32_t k = 0; k <= N / 2u; k++) {
        if (k == peak) continue;
        rest += sqrt((double)bins[k].re * bins[k].re +
                     (double)bins[k].im * bins[k].im);
    }
    CHECK(peak == 13u, "pure tone peaks at bin 13");
    CHECK(pmag > 1000.0 * rest, "all other bins are negligible (>60 dB down in sum)");

    /* Two tones, bins 13 and 40, different amplitudes. */
    for (uint32_t i = 0; i < N; i++)
        x[i] = (float)(cos(2.0 * M_PI * 13.0 * i / (double)N) +
                       0.5 * cos(2.0 * M_PI * 40.0 * i / (double)N));
    tessera_rfft(x, bins, g_tw, N);
    double m13 = sqrt((double)bins[13].re * bins[13].re + (double)bins[13].im * bins[13].im);
    double m40 = sqrt((double)bins[40].re * bins[40].re + (double)bins[40].im * bins[40].im);
    double others = 0.0;
    for (uint32_t k = 0; k <= N / 2u; k++) {
        if (k == 13u || k == 40u) continue;
        others += sqrt((double)bins[k].re * bins[k].re + (double)bins[k].im * bins[k].im);
    }
    CHECK(m13 > 100.0 * others && m40 > 50.0 * others,
          "two-tone: bins 13 and 40 dominate everything else");
    CHECK(fabs(m13 - 2.0 * m40) < 0.01 * m13,
          "amplitude ratio preserved (bin 13 twice bin 40)");
}

static void test_windows(void)
{
    printf("- periodic windows and their overlap-add identities\n");
    enum { W = 128 };
    float hann[W], hamm[W];
    tessera_window_hann(hann, W);
    tessera_window_hamming(hamm, W);

    CHECK(fabsf(hann[0]) < 1e-6f && fabsf(hann[W / 2] - 1.0f) < 1e-6f,
          "hann: 0 at the edge, 1 at the centre (periodic form)");
    CHECK(fabsf(hamm[0] - 0.08f) < 1e-6f && fabsf(hamm[W / 2] - 1.0f) < 1e-6f,
          "hamming: 0.08 at the edge, 1 at the centre");

    /* COLA at 50% overlap: hann[i] + hann[i + W/2] == 1 for all i. */
    int cola = 1;
    for (uint32_t i = 0; i < W / 2; i++)
        if (fabsf(hann[i] + hann[i + W / 2] - 1.0f) > 1e-5f) cola = 0;
    CHECK(cola, "hann sums to unity at 50% overlap (OLA-ready)");

    /* Hann^2 at 75% overlap sums to a constant (3/2). */
    int cola2 = 1;
    for (uint32_t i = 0; i < W / 4; i++) {
        float s = 0.0f;
        for (uint32_t h = 0; h < 4; h++) {
            float w = hann[i + h * (W / 4)];
            s += w * w;
        }
        if (fabsf(s - 1.5f) > 1e-5f) cola2 = 0;
    }
    CHECK(cola2, "hann^2 sums to 3/2 at 75% overlap (weighted-OLA-ready)");
}

static void test_guards(void)
{
    printf("- guards: non-power-of-two and tiny sizes are refused\n");
    tessera_cpx_t x[12];
    for (uint32_t i = 0; i < 12; i++) { x[i].re = 1.0f; x[i].im = 2.0f; }
    tessera_fft(x, g_tw, 12);                  /* not a power of two */
    int untouched = 1;
    for (uint32_t i = 0; i < 12; i++)
        if (x[i].re != 1.0f || x[i].im != 2.0f) untouched = 0;
    CHECK(untouched, "n=12 leaves the buffer untouched");
}

int main(void)
{
    printf("=== SDK FFT primitive host tests (issue #184) ===\n");
    tessera_fft_twiddles(g_tw, N);

    test_complex_vs_dft();
    test_roundtrip();
    test_rfft_vs_complex();
    test_rfft_roundtrip();
    test_tones();
    test_windows();
    test_guards();

    if (g_fail) {
        printf("FFT TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("FFT TESTS: ALL PASS\n");
    return 0;
}
