/* tests/arm64/morph_test.c - host unit tests for scene / parameter morphing
 * (Theme M17, issue #173).
 *
 * Build/run via:  make test-arm-morph
 */

#include "morph.h"

#include <stdio.h>
#include <math.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define NEAR(a, b, eps) (fabsf((a) - (b)) < (eps))

/* Parameter ids for the test. */
enum { P_LEVEL = 0, P_CUTOFF, P_MODE, P_ONLY_A, P_ONLY_B };

static void test_curves(void)
{
    printf("- interpolation curves\n");
    CHECK(NEAR(morph_interp(0.0f, 100.0f, 0.25f, MORPH_LINEAR), 25.0f, 0.01f),
          "linear: 0->100 at 0.25 = 25");
    /* Exp/geometric: 100 -> 400, halfway is the geometric mean 200 (an octave up
     * of the octave-up midpoint), not the arithmetic 250. */
    CHECK(NEAR(morph_interp(100.0f, 400.0f, 0.5f, MORPH_EXP), 200.0f, 1.0f),
          "exp: 100->400 at 0.5 = 200 (geometric mean)");
    CHECK(NEAR(morph_interp(100.0f, 1600.0f, 0.25f, MORPH_EXP), 200.0f, 1.5f),
          "exp: 100->1600 at 0.25 = 200 (one of four octaves)");
    CHECK(morph_interp(10.0f, 99.0f, 0.4f, MORPH_STEP) == 10.0f, "step: below 0.5 holds a");
    CHECK(morph_interp(10.0f, 99.0f, 0.6f, MORPH_STEP) == 99.0f, "step: at/above 0.5 takes b");
}

static void test_endpoints_and_hold(void)
{
    printf("- endpoints and topology mismatch\n");
    morph_snapshot_t a, b;
    morph_init(&a); morph_init(&b);
    morph_set(&a, P_LEVEL,  0.2f,   MORPH_LINEAR);
    morph_set(&a, P_CUTOFF, 200.0f, MORPH_EXP);
    morph_set(&a, P_ONLY_A, 0.9f,   MORPH_LINEAR);
    morph_set(&b, P_LEVEL,  0.8f,   MORPH_LINEAR);
    morph_set(&b, P_CUTOFF, 3200.0f, MORPH_EXP);
    morph_set(&b, P_ONLY_B, 0.1f,   MORPH_LINEAR);

    /* pos 0 -> a's values, pos 1 -> b's values. */
    CHECK(NEAR(morph_value(&a, &b, P_LEVEL, 0.0f, 0), 0.2f, 1e-4f), "pos 0 gives A's level");
    CHECK(NEAR(morph_value(&a, &b, P_LEVEL, 1.0f, 0), 0.8f, 1e-4f), "pos 1 gives B's level");
    CHECK(NEAR(morph_value(&a, &b, P_LEVEL, 0.5f, 0), 0.5f, 1e-4f), "pos 0.5 is halfway");
    /* Cutoff sweeps geometrically 200 -> 3200 (four octaves); halfway = 800. */
    CHECK(NEAR(morph_value(&a, &b, P_CUTOFF, 0.5f, 0), 800.0f, 2.0f),
          "cutoff morphs geometrically (200->3200 midpoint 800)");

    /* Parameters present in only one snapshot hold their value at any position. */
    CHECK(morph_value(&a, &b, P_ONLY_A, 0.7f, -1.0f) == 0.9f, "A-only param holds A's value");
    CHECK(morph_value(&a, &b, P_ONLY_B, 0.3f, -1.0f) == 0.1f, "B-only param holds B's value");
    CHECK(morph_value(&a, &b, 999u, 0.5f, -1.0f) == -1.0f, "absent param returns the default");
}

static void test_eval_union(void)
{
    printf("- morph_eval covers the union of both snapshots\n");
    morph_snapshot_t a, b;
    morph_init(&a); morph_init(&b);
    morph_set(&a, P_LEVEL, 0.0f, MORPH_LINEAR);
    morph_set(&a, P_ONLY_A, 5.0f, MORPH_LINEAR);
    morph_set(&b, P_LEVEL, 1.0f, MORPH_LINEAR);
    morph_set(&b, P_ONLY_B, 7.0f, MORPH_LINEAR);

    morph_param_t out[8];
    int n = morph_eval(&a, &b, 0.5f, out, 8);
    CHECK(n == 3, "union has 3 distinct params (LEVEL, ONLY_A, ONLY_B)");

    float level = 0, oa = 0, ob = 0; int seen = 0;
    for (int i = 0; i < n; i++) {
        if (out[i].id == P_LEVEL)  { level = out[i].value; seen |= 1; }
        if (out[i].id == P_ONLY_A) { oa = out[i].value;    seen |= 2; }
        if (out[i].id == P_ONLY_B) { ob = out[i].value;    seen |= 4; }
    }
    CHECK(seen == 7, "all three params present in the output");
    CHECK(NEAR(level, 0.5f, 1e-4f), "shared LEVEL is morphed to 0.5");
    CHECK(oa == 5.0f && ob == 7.0f, "one-sided params are held");
}

static void test_sweep_monotonic(void)
{
    printf("- a full 0->1 sweep is continuous and monotonic\n");
    morph_snapshot_t a, b;
    morph_init(&a); morph_init(&b);
    morph_set(&a, P_CUTOFF, 100.0f, MORPH_EXP);
    morph_set(&b, P_CUTOFF, 6400.0f, MORPH_EXP);

    float prev = 0.0f; int mono = 1; float maxjump = 0.0f;
    for (int i = 0; i <= 100; i++) {
        float pos = (float)i / 100.0f;
        float v = morph_value(&a, &b, P_CUTOFF, pos, 0);
        if (i > 0) {
            if (v < prev) mono = 0;
            float jump = fabsf(v - prev);
            if (jump > maxjump) maxjump = jump;
        }
        prev = v;
    }
    CHECK(mono, "the sweep never reverses (monotonic)");
    /* No step: the largest single step is a small fraction of the range. */
    CHECK(maxjump < (6400.0f - 100.0f) * 0.1f, "no discontinuity across the sweep");
}

int main(void)
{
    printf("=== Tessera scene/parameter morphing tests (M17, #173) ===\n");
    test_curves();
    test_endpoints_and_hold();
    test_eval_union();
    test_sweep_monotonic();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
