/* tests/arm64/conv_test.c - host unit tests for the SDK IR convolution engine
 * (Theme B, issue #112).
 *
 * Convolution is defined by a handful of exact identities, so the tests check
 * them directly against a brute-force reference: an impulse in reproduces the
 * IR, an identity IR is a pass-through, a delayed-impulse IR delays the signal,
 * and an arbitrary signal/IR pair matches the textbook convolution sum.
 *
 * Build/run via:  make test-arm-conv
 */

#include "tessera.h"

#include <stdio.h>
#include <stdint.h>
#include <math.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Brute-force reference: y[n] = sum_k ir[k] * x[n-k], x[<0] = 0. */
static float ref_conv(const float *x, int n, const float *ir, int ir_len, int i)
{
    float acc = 0.0f;
    for (int k = 0; k < ir_len; k++) {
        int j = i - k;
        if (j >= 0 && j < n) acc += ir[k] * x[j];
    }
    return acc;
}

static void test_impulse_reproduces_ir(void)
{
    printf("- an impulse input reproduces the impulse response\n");
    float ir[5] = { 0.5f, -0.25f, 0.125f, 1.0f, -0.75f };
    static float hist[8];
    tessera_conv_t c;
    tessera_conv_init(&c, ir, 5, hist, 8);

    int ok = 1;
    for (int i = 0; i < 5; i++) {
        float x = (i == 0) ? 1.0f : 0.0f;
        float y = tessera_conv(&c, x);
        if (fabsf(y - ir[i]) > 1e-6f) ok = 0;
    }
    CHECK(ok, "output samples equal the IR taps in order");
}

static void test_identity_passthrough(void)
{
    printf("- an identity IR ({1}) is a bit-exact pass-through\n");
    float ir[1] = { 1.0f };
    static float hist[4];
    tessera_conv_t c;
    tessera_conv_init(&c, ir, 1, hist, 4);
    int ok = 1;
    for (int i = 0; i < 16; i++) {
        float x = (float)(i * 7 % 11) - 5.0f;
        if (tessera_conv(&c, x) != x) ok = 0;
    }
    CHECK(ok, "y == x for every sample");
}

static void test_delay_ir(void)
{
    printf("- a delayed-impulse IR ({0,0,1}) delays the signal by 2 samples\n");
    float ir[3] = { 0.0f, 0.0f, 1.0f };
    static float hist[8];
    tessera_conv_t c;
    tessera_conv_init(&c, ir, 3, hist, 8);
    float in[6]  = { 3, 1, 4, 1, 5, 9 };
    float out[6];
    for (int i = 0; i < 6; i++) out[i] = tessera_conv(&c, in[i]);
    CHECK(out[0] == 0 && out[1] == 0, "first two outputs are silent (fill)");
    CHECK(out[2] == 3 && out[3] == 1 && out[4] == 4 && out[5] == 1,
          "the input reappears two samples late");
}

static void test_matches_reference(void)
{
    printf("- an arbitrary signal/IR pair matches the convolution sum\n");
    enum { N = 64, IR = 17 };
    float x[N], ir[IR];
    for (int i = 0; i < N; i++)  x[i]  = sinf(0.3f * i) + 0.5f * cosf(0.11f * i);
    for (int k = 0; k < IR; k++) ir[k] = expf(-0.25f * k) * (k % 2 ? -1.0f : 1.0f);

    static float hist[IR];
    tessera_conv_t c;
    tessera_conv_init(&c, ir, IR, hist, IR);

    float max_err = 0.0f;
    for (int i = 0; i < N; i++) {
        float y   = tessera_conv(&c, x[i]);
        float r   = ref_conv(x, N, ir, IR, i);
        float err = fabsf(y - r);
        if (err > max_err) max_err = err;
    }
    CHECK(max_err < 1e-5f, "matches brute-force convolution within 1e-5");
}

static void test_normgain(void)
{
    printf("- normgain is the sum of absolute taps (worst-case DC gain)\n");
    float ir[4] = { 0.5f, -0.5f, 1.0f, -0.25f };
    float g = tessera_conv_normgain(ir, 4);
    CHECK(fabsf(g - 2.25f) < 1e-6f, "sum(|ir|) == 2.25");
}

static void test_reset(void)
{
    printf("- reset clears history so a second pass is independent\n");
    float ir[3] = { 1.0f, 0.5f, 0.25f };
    static float hist[4];
    tessera_conv_t c;
    tessera_conv_init(&c, ir, 3, hist, 4);
    (void)tessera_conv(&c, 9.0f);
    (void)tessera_conv(&c, 9.0f);
    tessera_conv_reset(&c);
    /* After reset, an impulse must again reproduce ir[0] with no residue. */
    CHECK(tessera_conv(&c, 1.0f) == ir[0], "post-reset impulse -> ir[0] exactly");
}

int main(void)
{
    printf("=== Tessera SDK IR-convolution tests (Theme B, #112) ===\n");
    test_impulse_reproduces_ir();
    test_identity_passthrough();
    test_delay_ir();
    test_matches_reference();
    test_normgain();
    test_reset();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
