/* tests/arm64/pconv_test.c - host unit tests for partitioned FFT convolution
 * (Theme M18, issue #185).
 *
 * The partitioned engine must be *the same filter* as the direct one, just
 * cheaper: its streaming output is compared against both a brute-force
 * double-precision linear convolution and the existing per-sample
 * tessera_conv engine, across IR lengths that exercise every partition-count
 * case (shorter than a block, exactly one block, a non-multiple, many
 * partitions).  A delta IR must pass the signal through bit-nearly, a shifted
 * delta must delay it, and reset must clear all history.
 *
 * Build/run via:  make test-arm-pconv
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

static uint32_t g_seed = 0xbeefcafeu;
static float frand(void)
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return (float)(int32_t)g_seed / 2147483648.0f;
}

#define BLOCK   64u
#define NBLK    10u
#define NSAMP   (BLOCK * NBLK)
#define MAX_IR  512u
#define MAX_P   ((MAX_IR + BLOCK - 1) / BLOCK)
#define BINS    (BLOCK + 1u)

/* Caller-owned pconv buffers, sized for the worst case. */
static tessera_cpx_t g_tw[BLOCK];
static tessera_cpx_t g_h[MAX_P * BINS];
static tessera_cpx_t g_x[MAX_P * BINS];
static tessera_cpx_t g_acc[BINS];
static float         g_work[4u * BLOCK];

/* Brute-force reference in double. */
static void conv_ref(const float *x, uint32_t nx, const float *ir, uint32_t m,
                     float *y)
{
    for (uint32_t n = 0; n < nx; n++) {
        double acc = 0.0;
        for (uint32_t k = 0; k < m && k <= n; k++)
            acc += (double)ir[k] * (double)x[n - k];
        y[n] = (float)acc;
    }
}

static float run_and_maxerr(const float *x, const float *ir, uint32_t ir_len)
{
    static float want[NSAMP], got[NSAMP];
    conv_ref(x, NSAMP, ir, ir_len, want);

    tessera_pconv_t pc;
    if (tessera_pconv_init(&pc, BLOCK, ir, ir_len, g_tw,
                           g_h, g_x, g_acc, g_work) != 0)
        return 1e9f;
    for (uint32_t b = 0; b < NBLK; b++)
        tessera_pconv_process(&pc, x + b * BLOCK, got + b * BLOCK);

    float m = 0.0f;
    for (uint32_t i = 0; i < NSAMP; i++) {
        float d = fabsf(got[i] - want[i]);
        if (d > m) m = d;
    }
    return m;
}

static void test_vs_reference(void)
{
    printf("- streaming output matches brute-force linear convolution\n");
    static float x[NSAMP], ir[MAX_IR];
    for (uint32_t i = 0; i < NSAMP; i++) x[i] = frand();
    for (uint32_t i = 0; i < MAX_IR; i++) ir[i] = frand() * 0.2f;

    struct { uint32_t len; const char *what; } cases[] = {
        { 17u,       "IR shorter than a block (1 partition, padded)" },
        { BLOCK,     "IR exactly one block" },
        { 200u,      "IR a non-multiple of the block (4 partitions)" },
        { MAX_IR,    "long IR (8 partitions)" },
    };
    for (uint32_t c = 0; c < 4; c++) {
        float err = run_and_maxerr(x, ir, cases[c].len);
        printf("    ir_len=%u max err = %.2e\n", cases[c].len, (double)err);
        CHECK(err < 2e-4f, cases[c].what);
    }
}

static void test_vs_direct_engine(void)
{
    printf("- matches the direct tessera_conv engine sample for sample\n");
    static float x[NSAMP], ir[200], hist[256], direct[NSAMP], part[NSAMP];
    for (uint32_t i = 0; i < NSAMP; i++) x[i] = frand();
    for (uint32_t i = 0; i < 200; i++) ir[i] = frand() * 0.2f;

    tessera_conv_t c;
    tessera_conv_init(&c, ir, 200, hist, 256);
    for (uint32_t i = 0; i < NSAMP; i++)
        direct[i] = tessera_conv(&c, x[i]);

    tessera_pconv_t pc;
    tessera_pconv_init(&pc, BLOCK, ir, 200, g_tw, g_h, g_x, g_acc, g_work);
    for (uint32_t b = 0; b < NBLK; b++)
        tessera_pconv_process(&pc, x + b * BLOCK, part + b * BLOCK);

    float m = 0.0f;
    for (uint32_t i = 0; i < NSAMP; i++) {
        float d = fabsf(direct[i] - part[i]);
        if (d > m) m = d;
    }
    printf("    max |direct - partitioned| = %.2e\n", (double)m);
    CHECK(m < 2e-4f, "the two engines are the same filter");
}

static void test_delta(void)
{
    printf("- delta IRs: identity and pure delay\n");
    static float x[NSAMP], y[NSAMP], ir[BLOCK + 1u];

    for (uint32_t i = 0; i < NSAMP; i++) x[i] = frand();

    /* ir = delta -> output == input. */
    memset(ir, 0, sizeof ir);
    ir[0] = 1.0f;
    tessera_pconv_t pc;
    tessera_pconv_init(&pc, BLOCK, ir, 1u, g_tw, g_h, g_x, g_acc, g_work);
    for (uint32_t b = 0; b < NBLK; b++)
        tessera_pconv_process(&pc, x + b * BLOCK, y + b * BLOCK);
    float m = 0.0f;
    for (uint32_t i = 0; i < NSAMP; i++) {
        float d = fabsf(y[i] - x[i]);
        if (d > m) m = d;
    }
    CHECK(m < 1e-5f, "delta IR passes the signal through (zero added latency)");

    /* ir = delta at exactly one block -> output is input delayed by a block
     * (this crosses the partition boundary: partition 1, tap 0). */
    memset(ir, 0, sizeof ir);
    ir[BLOCK] = 1.0f;
    tessera_pconv_init(&pc, BLOCK, ir, BLOCK + 1u, g_tw, g_h, g_x, g_acc, g_work);
    for (uint32_t b = 0; b < NBLK; b++)
        tessera_pconv_process(&pc, x + b * BLOCK, y + b * BLOCK);
    m = 0.0f;
    for (uint32_t i = BLOCK; i < NSAMP; i++) {
        float d = fabsf(y[i] - x[i - BLOCK]);
        if (d > m) m = d;
    }
    int head_silent = 1;
    for (uint32_t i = 0; i < BLOCK; i++)
        if (fabsf(y[i]) > 1e-5f) head_silent = 0;
    CHECK(m < 1e-5f && head_silent, "delta at tap B delays by exactly one block");
}

static void test_reset(void)
{
    printf("- reset clears all history\n");
    static float x[NSAMP], y1[NSAMP], y2[NSAMP], ir[200];
    for (uint32_t i = 0; i < NSAMP; i++) x[i] = frand();
    for (uint32_t i = 0; i < 200; i++) ir[i] = frand() * 0.2f;

    tessera_pconv_t pc;
    tessera_pconv_init(&pc, BLOCK, ir, 200, g_tw, g_h, g_x, g_acc, g_work);
    for (uint32_t b = 0; b < NBLK; b++)
        tessera_pconv_process(&pc, x + b * BLOCK, y1 + b * BLOCK);

    tessera_pconv_reset(&pc);
    for (uint32_t b = 0; b < NBLK; b++)
        tessera_pconv_process(&pc, x + b * BLOCK, y2 + b * BLOCK);

    int same = 1;
    for (uint32_t i = 0; i < NSAMP; i++)
        if (y1[i] != y2[i]) same = 0;
    CHECK(same, "post-reset run is bit-identical to the first run");
}

static void test_guards(void)
{
    printf("- guards\n");
    static float ir[8] = { 1.0f };
    tessera_pconv_t pc;
    CHECK(tessera_pconv_init(&pc, 48u, ir, 8, g_tw, g_h, g_x, g_acc, g_work) == -1,
          "non-power-of-two block refused");
    CHECK(tessera_pconv_init(&pc, BLOCK, ir, 0, g_tw, g_h, g_x, g_acc, g_work) == -1,
          "empty IR refused");
    CHECK(tessera_pconv_parts(BLOCK, 1u) == 1u &&
          tessera_pconv_parts(BLOCK, BLOCK) == 1u &&
          tessera_pconv_parts(BLOCK, BLOCK + 1u) == 2u &&
          tessera_pconv_parts(BLOCK, 512u) == 8u,
          "partition count: ceil(ir_len / block)");
}

int main(void)
{
    printf("=== partitioned FFT convolution host tests (issue #185) ===\n");
    tessera_fft_twiddles(g_tw, 2u * BLOCK);

    test_vs_reference();
    test_vs_direct_engine();
    test_delta();
    test_reset();
    test_guards();

    if (g_fail) {
        printf("PCONV TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("PCONV TESTS: ALL PASS\n");
    return 0;
}
