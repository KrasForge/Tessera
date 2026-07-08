/* tests/arm64/src_fir_test.c - host unit tests for the polyphase-FIR
 * sample-rate converter (Theme M20, issue #192).
 *
 * Head-to-head against the linear-interpolation SRC (issue #131) on the same
 * inputs, with DFT probes on the outputs:
 *   - passband: tones stay within a fraction of a dB through the FIR, where
 *     linear interpolation audibly droops the top octave;
 *   - stopband: upsampling images and downsampling aliases are suppressed
 *     far better than linear interpolation manages;
 *   - DC and the rate-ratio output count match the existing SRC;
 *   - streaming across blocks is seam-free (bit-identical to one-shot).
 *
 * Build/run via:  make test-arm-src-fir
 */

#include "src_fir.h"
#include "src.h"

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

/* Goertzel amplitude of a tone at `f` (sample rate sr). */
static double tone_amp(const int16_t *x, uint32_t n, double f, double sr)
{
    double w = 2.0 * M_PI * f / sr;
    double c = 2.0 * cos(w), s0, s1 = 0.0, s2 = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        s0 = (double)x[i] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 2.0 * sqrt(p > 0.0 ? p : 0.0) / (double)n;
}

#define NIN  48000u
static int16_t g_in[NIN];
static int16_t g_fir[2u * NIN + 64u];
static int16_t g_lin[2u * NIN + 64u];

static int run_fir(uint32_t rin, uint32_t rout, const int16_t *in, uint32_t n,
                   int16_t *out)
{
    src_fir_t s;
    src_fir_init(&s, rin, rout);
    return src_fir_process(&s, in, (int)n, out, 2 * (int)NIN + 64);
}

static int run_lin(uint32_t rin, uint32_t rout, const int16_t *in, uint32_t n,
                   int16_t *out)
{
    src_t s;
    src_init(&s, rin, rout);
    return src_process(&s, in, (int)n, out, 2 * (int)NIN + 64);
}

/* ---- DC and output count ---------------------------------------------------- */

static void test_dc_and_count(void)
{
    printf("- DC is exact and the output count matches the linear SRC\n");
    for (uint32_t i = 0; i < NIN; i++)
        g_in[i] = 12000;

    int nf = run_fir(48000, 44100, g_in, NIN, g_fir);
    int nl = run_lin(48000, 44100, g_in, NIN, g_lin);
    printf("    48k->44.1k: fir=%d lin=%d outputs\n", nf, nl);
    CHECK(nf > 0 && nl > 0 && (nf > nl ? nf - nl : nl - nf) <= 20,
          "output count within a group delay of the linear SRC");

    int exact = 1;
    for (int i = 64; i < nf; i++)          /* skip the priming transient */
        if (g_fir[i] != 12000) exact = 0;
    CHECK(exact, "DC passes bit-exactly (per-phase sums normalised to 32768)");
}

/* ---- streaming seam --------------------------------------------------------- */

static void test_streaming_seam(void)
{
    printf("- chunked processing is bit-identical to one-shot\n");
    uint32_t seed = 0x51c0ffeeu;
    for (uint32_t i = 0; i < NIN; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        g_in[i] = (int16_t)(seed & 0x7fffu) - 16384;
    }
    int nf = run_fir(48000, 44100, g_in, NIN, g_fir);

    src_fir_t s;
    src_fir_init(&s, 48000, 44100);
    int nc = 0;
    uint32_t off = 0;
    uint32_t chunks[] = { 1, 63, 64, 480, 7, 1000, 129 };
    uint32_t ci = 0;
    while (off < NIN) {
        uint32_t n = chunks[ci++ % 7u];
        if (n > NIN - off) n = NIN - off;
        nc += src_fir_process(&s, g_in + off, (int)n,
                              g_lin + nc, 2 * (int)NIN + 64 - nc);
        off += n;
    }
    CHECK(nc == nf, "same output count");
    CHECK(memcmp(g_fir, g_lin, (size_t)nf * sizeof(int16_t)) == 0,
          "bit-identical stream across arbitrary chunk boundaries");
}

/* ---- passband flatness ------------------------------------------------------- */

static void test_passband(void)
{
    printf("- passband: flat where linear interpolation droops\n");
    /* Upsample 24k -> 48k; probe a tone at 9.6 kHz = 0.8 of the input
     * Nyquist, where linear interpolation loses over a dB. */
    double f = 9600.0;
    for (uint32_t i = 0; i < NIN; i++)
        g_in[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * f * i / 24000.0));

    int nf = run_fir(24000, 48000, g_in, NIN, g_fir);
    int nl = run_lin(24000, 48000, g_in, NIN, g_lin);

    /* Measure over the steady middle. */
    double af = tone_amp(g_fir + 4096, (uint32_t)nf - 8192, f, 48000.0);
    double al = tone_amp(g_lin + 4096, (uint32_t)nl - 8192, f, 48000.0);
    double df = 20.0 * log10(af / 20000.0);
    double dl = 20.0 * log10(al / 20000.0);
    printf("    0.8 Nyquist tone: fir %+.2f dB, linear %+.2f dB\n", df, dl);
    CHECK(fabs(df) < 1.0, "FIR passband within 1 dB at 0.8 Nyquist");
    CHECK(fabs(df) < fabs(dl), "flatter than linear interpolation");
    CHECK(dl < -1.0, "(the linear SRC really does droop here)");
}

/* ---- image rejection (upsampling) --------------------------------------------- */

static void test_images(void)
{
    printf("- upsampling images suppressed far better than linear\n");
    /* 24k -> 48k with a 7 kHz tone: the spectral image lands at 17 kHz. */
    double f = 7000.0, fi = 24000.0 - 7000.0;
    for (uint32_t i = 0; i < NIN; i++)
        g_in[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * f * i / 24000.0));

    int nf = run_fir(24000, 48000, g_in, NIN, g_fir);
    int nl = run_lin(24000, 48000, g_in, NIN, g_lin);

    double tf = tone_amp(g_fir + 4096, (uint32_t)nf - 8192, f, 48000.0);
    double xf = tone_amp(g_fir + 4096, (uint32_t)nf - 8192, fi, 48000.0);
    double tl = tone_amp(g_lin + 4096, (uint32_t)nl - 8192, f, 48000.0);
    double xl = tone_amp(g_lin + 4096, (uint32_t)nl - 8192, fi, 48000.0);
    double rej_f = 20.0 * log10(tf / (xf + 1e-9));
    double rej_l = 20.0 * log10(tl / (xl + 1e-9));
    printf("    image rejection: fir %.1f dB, linear %.1f dB\n", rej_f, rej_l);
    CHECK(rej_f > 40.0, "FIR rejects the 17 kHz image by > 40 dB");
    CHECK(rej_f > rej_l + 15.0, "at least 15 dB better than linear");
}

/* ---- alias rejection (downsampling) -------------------------------------------- */

static void test_aliases(void)
{
    printf("- downsampling aliases suppressed (cutoff scales with the ratio)\n");
    /* 48k -> 24k with a 9 kHz tone: above the output Nyquist (12k)?  No -
     * use 15 kHz, which would alias to 24 - 15 = 9 kHz in the output. */
    double f = 15000.0, fa = 24000.0 - 15000.0;
    for (uint32_t i = 0; i < NIN; i++)
        g_in[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * f * i / 48000.0));

    int nf = run_fir(48000, 24000, g_in, NIN, g_fir);
    int nl = run_lin(48000, 24000, g_in, NIN, g_lin);

    double af = tone_amp(g_fir + 2048, (uint32_t)nf - 4096, fa, 24000.0);
    double al = tone_amp(g_lin + 2048, (uint32_t)nl - 4096, fa, 24000.0);
    double sf = 20.0 * log10(af / 20000.0);
    double sl = 20.0 * log10(al / 20000.0);
    printf("    alias level: fir %.1f dBFS, linear %.1f dBFS\n", sf, sl);
    CHECK(sf < -40.0, "FIR alias below -40 dBFS");
    CHECK(sf < sl - 15.0, "at least 15 dB below linear's alias");

    /* And a tone inside the (scaled) passband survives. */
    double fp = 6000.0;
    for (uint32_t i = 0; i < NIN; i++)
        g_in[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * fp * i / 48000.0));
    nf = run_fir(48000, 24000, g_in, NIN, g_fir);
    double ap = tone_amp(g_fir + 2048, (uint32_t)nf - 4096, fp, 24000.0);
    printf("    6 kHz through 48k->24k: %+.2f dB\n", 20.0 * log10(ap / 20000.0));
    CHECK(fabs(20.0 * log10(ap / 20000.0)) < 1.0,
          "in-band tone passes within 1 dB");
}

/* ---- odd ratio sanity ---------------------------------------------------------- */

static void test_odd_ratio(void)
{
    printf("- a non-trivial ratio (44.1k -> 48k) reproduces its tone\n");
    double f = 1000.0;
    for (uint32_t i = 0; i < NIN; i++)
        g_in[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * f * i / 44100.0));
    int nf = run_fir(44100, 48000, g_in, NIN, g_fir);
    CHECK(nf > (int)(NIN) && nf < (int)(NIN * 48000ull / 44100ull) + 8,
          "output count ~ n_in * 48000/44100");
    double a = tone_amp(g_fir + 4096, (uint32_t)nf - 8192, f, 48000.0);
    CHECK(fabs(20.0 * log10(a / 20000.0)) < 0.5,
          "1 kHz tone amplitude preserved");
}

int main(void)
{
    printf("=== polyphase-FIR SRC host tests (issue #192) ===\n");
    test_dc_and_count();
    test_streaming_seam();
    test_passband();
    test_images();
    test_aliases();
    test_odd_ratio();

    if (g_fail) {
        printf("SRC-FIR TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("SRC-FIR TESTS: ALL PASS\n");
    return 0;
}
