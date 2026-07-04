/* tests/arm64/src_test.c - host unit tests for the sample-rate converter
 * (Theme H, issue #131).
 *
 * Build/run via:  make test-arm-src
 */

#include "src.h"

#include <stdio.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static void test_dc(void)
{
    printf("- a constant (DC) signal is preserved exactly\n");
    src_t s; src_init(&s, 48000, 96000);
    int16_t in[64], out[256];
    for (int i = 0; i < 64; i++) in[i] = 1234;
    int n = src_process(&s, in, 64, out, 256);
    int ok = n > 0;
    for (int i = 0; i < n; i++) if (out[i] != 1234) ok = 0;
    CHECK(ok, "every output sample equals the DC input");
}

static void test_upsample_2x(void)
{
    printf("- 2x upsample interpolates linear midpoints\n");
    src_t s; src_init(&s, 1, 2);            /* exact 2x */
    int16_t in[] = { 0, 10, 20, 30 };
    int16_t out[16];
    int n = src_process(&s, in, 4, out, 16);
    /* Windows [0,10),[10,20),[20,30) each give {left, midpoint}. */
    CHECK(n == 6, "3 windows x 2 = 6 outputs");
    CHECK(out[0] == 0 && out[1] == 5 && out[2] == 10 &&
          out[3] == 15 && out[4] == 20 && out[5] == 25,
          "outputs are 0,5,10,15,20,25 (exact linear)");
}

static void test_downsample_2x(void)
{
    printf("- 2x downsample keeps every other sample\n");
    src_t s; src_init(&s, 2, 1);            /* exact 1/2 */
    int16_t in[] = { 0, 10, 20, 30, 40 };
    int16_t out[16];
    int n = src_process(&s, in, 5, out, 16);
    CHECK(n == 2, "5 inputs -> 2 outputs at half rate");
    CHECK(out[0] == 0 && out[1] == 20, "kept samples 0 and 2");
}

static void test_ratios(void)
{
    printf("- output count tracks the rate ratio\n");
    struct { uint32_t in, out; } cases[] = {
        { 48000, 96000 }, { 96000, 48000 }, { 44100, 48000 }, { 48000, 44100 },
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        src_t s; src_init(&s, cases[c].in, cases[c].out);
        enum { N = 4800 };
        static int16_t in[N], out[N * 3];
        for (int i = 0; i < N; i++) in[i] = (int16_t)(i % 100);
        int cap = src_out_capacity(&s, N);
        int n = src_process(&s, in, N, out, N * 3);
        /* Expected ~ N * out/in, within a couple of samples. */
        int expect = (int)((int64_t)N * cases[c].out / cases[c].in);
        char msg[64];
        snprintf(msg, sizeof msg, "%u->%u yields ~%d outputs", cases[c].in, cases[c].out, expect);
        CHECK(n >= expect - 2 && n <= expect + 2, msg);
        CHECK(n <= cap, "actual output fits the reported capacity");
    }
}

static void test_streaming_seam(void)
{
    printf("- block-by-block resampling matches a single-shot call\n");
    enum { N = 300 };
    int16_t in[N];
    for (int i = 0; i < N; i++) in[i] = (int16_t)(i * 7 % 200 - 100);

    /* Whole. */
    src_t a; src_init(&a, 44100, 48000);
    int16_t whole[N * 2];
    int nw = src_process(&a, in, N, whole, N * 2);

    /* Split into two blocks through the same converter state. */
    src_t b; src_init(&b, 44100, 48000);
    int16_t parts[N * 2];
    int np = src_process(&b, in, 128, parts, N * 2);
    np += src_process(&b, in + 128, N - 128, parts + np, N * 2 - np);

    int ok = (nw == np);
    for (int i = 0; i < nw && i < np; i++) if (whole[i] != parts[i]) ok = 0;
    CHECK(ok, "the seam between blocks is identical to the unsplit output");
}

int main(void)
{
    printf("=== Tessera sample-rate-conversion tests (Theme H, #131) ===\n");
    test_dc();
    test_upsample_2x();
    test_downsample_2x();
    test_ratios();
    test_streaming_seam();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
