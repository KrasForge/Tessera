/* tests/arm64/wtfm_test.c - host unit tests for the wavetable and FM
 * oscillators (Theme M15, issue #164).
 *
 * Uses a Goertzel probe to check spectral content: the wavetable plays the
 * stored waveform at the right pitch and its mip stack suppresses aliasing, and
 * the FM operator produces the expected sidebands.
 *
 * Build/run via:  make test-arm-wtfm
 */

#include "tessera.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define SR 48000.0f
#define PI_D 3.14159265358979323846

/* Goertzel magnitude at frequency `hz` over `n` samples. */
static double mag_at(const float *x, int n, double hz)
{
    double w = 2.0 * PI_D * hz / SR;
    double c = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; i++) { double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    double re = s1 - s2 * cos(w);
    double im = s2 * sin(w);
    return sqrt(re * re + im * im) / n;
}

/* Dominant frequency via interpolated upward zero-crossings. */
static float est_hz(const float *x, int n)
{
    float first = -1, last = -1; int count = 0;
    for (int i = 1; i < n; i++)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            float d = x[i] - x[i - 1];
            float pos = (float)(i - 1) + (d != 0.0f ? -x[i - 1] / d : 0.0f);
            if (first < 0) first = pos;
            last = pos; count++;
        }
    return (count >= 2 && last > first) ? (float)(count - 1) * SR / (last - first) : 0.0f;
}

#define TLEN 2048

static void test_wavetable_pitch(void)
{
    printf("- wavetable plays the stored waveform at the set pitch\n");
    static float table[TLEN];
    tessera_wt_bandlimit(table, TLEN, 1);          /* 1 harmonic = pure sine */
    const float *tables[1] = { table };
    tessera_wavetable_t wt;
    tessera_wt_init(&wt, tables, 1, TLEN, 20.0f, SR);
    tessera_wt_set_freq(&wt, 440.0f);

    static float out[8192];
    for (int i = 0; i < 8192; i++) out[i] = tessera_wt_process(&wt);
    float hz = est_hz(out, 8192);
    CHECK(fabsf(hz - 440.0f) < 2.0f, "sine table renders at ~440 Hz");

    float peak = 0;
    for (int i = 0; i < 8192; i++) if (fabsf(out[i]) > peak) peak = fabsf(out[i]);
    CHECK(peak > 0.9f && peak < 1.1f, "amplitude is roughly unit");
}

static void test_wavetable_selection(void)
{
    printf("- the mip stack selects a table by octave\n");
    static float t0[TLEN], t1[TLEN], t2[TLEN];
    tessera_wt_bandlimit(t0, TLEN, 64);
    tessera_wt_bandlimit(t1, TLEN, 32);
    tessera_wt_bandlimit(t2, TLEN, 16);
    const float *tables[3] = { t0, t1, t2 };
    tessera_wavetable_t wt;
    tessera_wt_init(&wt, tables, 3, TLEN, 100.0f, SR);   /* base 100 Hz */

    tessera_wt_set_freq(&wt, 120.0f);  CHECK(wt.sel == 0, "120 Hz -> table 0 (first octave)");
    tessera_wt_set_freq(&wt, 250.0f);  CHECK(wt.sel == 1, "250 Hz -> table 1 (second octave)");
    tessera_wt_set_freq(&wt, 900.0f);  CHECK(wt.sel == 2, "900 Hz -> table 2 (clamped to last)");
}

#define AA_N   16384
#define AA_BIN 796                       /* fundamental at an exact FFT bin */

/* Aliasing shows up as energy *below* the fundamental, where a band-limited tone
 * has no legitimate component.  With an integer-periodic fundamental, summing
 * exact FFT bins below it is leakage-free, so it measures aliasing only. */
static double alias_energy(const float *x)
{
    double sum = 0.0;
    for (int j = 10; j < AA_BIN; j++)
        sum += mag_at(x, AA_N, (double)j * SR / AA_N);
    return sum;
}

static void test_wavetable_antialiasing(void)
{
    printf("- band-limited mip stack suppresses aliasing vs a naive table\n");
    /* Fundamental sits on FFT bin 796 (~2332 Hz) so both the harmonics and any
     * aliased images land exactly on bins with no spectral leakage. */
    const float f0 = (float)AA_BIN * SR / AA_N;

    /* Naive: a single table crammed with 60 harmonics (many alias at f0). */
    static float naive[TLEN];
    tessera_wt_bandlimit(naive, TLEN, 60);
    const float *nt[1] = { naive };
    tessera_wavetable_t wn; tessera_wt_init(&wn, nt, 1, TLEN, 20.0f, SR);
    tessera_wt_set_freq(&wn, f0);
    static float on[AA_N];
    for (int i = 0; i < AA_N; i++) on[i] = tessera_wt_process(&wn);
    double alias_naive = alias_energy(on);

    /* Band-limited: per-octave stack where each table's highest harmonic stays
     * below Nyquist for the top of its octave. */
    static float mip[10][TLEN];
    const float *mt[10];
    for (int i = 0; i < 10; i++) {
        float octave_top = 20.0f * (float)(1 << (i + 1));
        int nh = (int)((SR * 0.5f) / octave_top);
        if (nh < 1) nh = 1;
        if (nh > TLEN / 2 - 1) nh = TLEN / 2 - 1;
        tessera_wt_bandlimit(mip[i], TLEN, nh);
        mt[i] = mip[i];
    }
    tessera_wavetable_t wm; tessera_wt_init(&wm, mt, 10, TLEN, 20.0f, SR);
    tessera_wt_set_freq(&wm, f0);
    static float om[AA_N];
    for (int i = 0; i < AA_N; i++) om[i] = tessera_wt_process(&wm);
    double alias_mip = alias_energy(om);

    CHECK(alias_naive > 1e-3, "the naive table aliases audibly below the fundamental");
    CHECK(alias_mip < alias_naive * 0.2, "the mip stack cuts the alias by >5x");
}

static void test_fm_sidebands(void)
{
    printf("- FM produces sidebands that grow with the modulation index\n");
    const float fc = 1000.0f;

    /* Index 0: pure carrier, no energy at the 2nd/3rd harmonic. */
    tessera_fm_op_t carrier = {0}, mod = {0};
    tessera_fm_op_set(&carrier, SR, fc);
    tessera_fm_op_set(&mod, SR, fc);              /* ratio 1:1 */
    static float o0[16384];
    for (int i = 0; i < 16384; i++) o0[i] = tessera_fm2(&carrier, &mod, 0.0f);
    double f1_0 = mag_at(o0, 16384, fc);
    double f2_0 = mag_at(o0, 16384, 2.0f * fc);

    CHECK(f1_0 > 0.1, "index 0 has a strong fundamental");
    CHECK(f2_0 < f1_0 * 0.02, "index 0 has essentially no 2nd harmonic");

    /* Index > 0: sidebands appear at 2*fc and 3*fc. */
    tessera_fm_op_t c2 = {0}, m2 = {0};
    tessera_fm_op_set(&c2, SR, fc);
    tessera_fm_op_set(&m2, SR, fc);
    static float o1[16384];
    for (int i = 0; i < 16384; i++) o1[i] = tessera_fm2(&c2, &m2, 0.5f);
    double f2_1 = mag_at(o1, 16384, 2.0f * fc);
    double f3_1 = mag_at(o1, 16384, 3.0f * fc);

    CHECK(f2_1 > f2_0 * 10.0, "a nonzero index adds a strong 2nd harmonic");
    CHECK(f3_1 > 1e-3, "and a 3rd harmonic appears too");
}

int main(void)
{
    printf("=== Tessera wavetable/FM oscillator tests (M15, #164) ===\n");
    test_wavetable_pitch();
    test_wavetable_selection();
    test_wavetable_antialiasing();
    test_fm_sidebands();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
