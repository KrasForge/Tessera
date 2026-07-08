/* tests/arm64/modmatrix_test.c - host unit tests for the modulation matrix
 * (Theme M19, issue #188).
 *
 * The acceptance criteria, verified directly:
 *   - a single LFO->cutoff route sweeps the destination between the expected
 *     bounds at the LFO rate (the LFO is a real SDK oscillator run at control
 *     rate, exactly how a synth would drive it);
 *   - multiple sources to one destination sum with independent depths and
 *     clamp to the destination range;
 *   - a route with depth 0, or a removed route, leaves the destination at
 *     its base value;
 *   - curves (EXP, INV) shape the source before the depth is applied.
 *
 * Build/run via:  make test-arm-modmatrix
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

#define MAXR 16u
#define NSRC 4u
#define NDST 4u

/* Source slots, as a synth would lay them out. */
enum { SRC_LFO = 0, SRC_ENV = 1, SRC_VEL = 2, SRC_PEDAL = 3 };
/* Destination slots. */
enum { DST_CUTOFF = 0, DST_PITCH = 1, DST_AMP = 2, DST_PAN = 3 };

static tessera_mod_route_t g_routes[MAXR];
static float               g_sources[NSRC];
static tessera_mod_dest_t  g_dests[NDST];

static void fresh(tessera_mod_t *m)
{
    tessera_mod_init(m, g_routes, MAXR, g_sources, NSRC, g_dests, NDST);
    tessera_mod_dest_setup(m, DST_CUTOFF, 2000.0f, 20.0f, 18000.0f);
    tessera_mod_dest_setup(m, DST_PITCH, 0.0f, -24.0f, 24.0f);
    tessera_mod_dest_setup(m, DST_AMP, 0.8f, 0.0f, 1.0f);
    tessera_mod_dest_setup(m, DST_PAN, 0.0f, -1.0f, 1.0f);
}

static void test_lfo_sweep(void)
{
    printf("- a real LFO->cutoff route sweeps between the expected bounds\n");
    tessera_mod_t m;
    fresh(&m);
    CHECK(tessera_mod_route(&m, SRC_LFO, DST_CUTOFF, 1500.0f, TESSERA_MOD_LIN) >= 0,
          "route added");

    /* A 2 Hz LFO evaluated at control rate: 48 kHz / 64-sample blocks =
     * 750 evals/s -> one LFO period = 375 blocks. */
    tessera_osc_t lfo = { 0 };
    tessera_osc_set(&lfo, 750.0f, 2.0f);

    float lo = 1e9f, hi = -1e9f;
    int   in_range = 1;
    float prev = 0.0f;
    int   crossings = 0;
    for (int b = 0; b < 750; b++) {                 /* two LFO periods */
        float s = tessera_osc_sin(&lfo);
        tessera_mod_set_source(&m, SRC_LFO, s);
        tessera_mod_eval(&m);
        float v = tessera_mod_value(&m, DST_CUTOFF);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        if (v < 20.0f || v > 18000.0f) in_range = 0;
        float c = v - 2000.0f;
        if (b > 0 && ((prev < 0.0f && c >= 0.0f) || (prev > 0.0f && c <= 0.0f)))
            crossings++;
        prev = c;
    }
    printf("    swept [%.0f, %.0f] Hz, %d centre crossings\n",
           (double)lo, (double)hi, crossings);
    CHECK(lo < 520.0f && lo > 480.0f, "bottom of the sweep ~ base - depth (500)");
    CHECK(hi > 3480.0f && hi < 3520.0f, "top of the sweep ~ base + depth (3500)");
    /* Two periods starting exactly ON the centre: crossings at 1/2, 1, and
     * 3/2 periods land inside the window (the start and end points sit on
     * the centre and are not sign changes). */
    CHECK(crossings == 3, "sweep crosses the base at the LFO rate");
    CHECK(in_range, "always inside the destination range");
}

static void test_sum_and_clamp(void)
{
    printf("- multiple sources sum with independent depths and clamp\n");
    tessera_mod_t m;
    fresh(&m);
    tessera_mod_route(&m, SRC_LFO, DST_CUTOFF, 1000.0f, TESSERA_MOD_LIN);
    tessera_mod_route(&m, SRC_ENV, DST_CUTOFF, 4000.0f, TESSERA_MOD_LIN);

    tessera_mod_set_source(&m, SRC_LFO, 0.5f);
    tessera_mod_set_source(&m, SRC_ENV, 1.0f);
    tessera_mod_eval(&m);
    CHECK(fabsf(tessera_mod_value(&m, DST_CUTOFF) - 6500.0f) < 1e-3f,
          "2000 + 0.5*1000 + 1.0*4000 = 6500");

    /* Push the sum past the ceiling: clamps to hi. */
    tessera_mod_route(&m, SRC_PEDAL, DST_CUTOFF, 50000.0f, TESSERA_MOD_LIN);
    tessera_mod_set_source(&m, SRC_PEDAL, 1.0f);
    tessera_mod_eval(&m);
    CHECK(tessera_mod_value(&m, DST_CUTOFF) == 18000.0f, "sum clamps to hi");

    /* Negative depth pushes below the floor: clamps to lo. */
    tessera_mod_t m2;
    fresh(&m2);
    tessera_mod_route(&m2, SRC_ENV, DST_CUTOFF, -50000.0f, TESSERA_MOD_LIN);
    tessera_mod_set_source(&m2, SRC_ENV, 1.0f);
    tessera_mod_eval(&m2);
    CHECK(tessera_mod_value(&m2, DST_CUTOFF) == 20.0f, "sum clamps to lo");
}

static void test_zero_and_removed(void)
{
    printf("- depth 0 and removed routes leave the base value\n");
    tessera_mod_t m;
    fresh(&m);
    int r = tessera_mod_route(&m, SRC_LFO, DST_CUTOFF, 0.0f, TESSERA_MOD_LIN);
    tessera_mod_set_source(&m, SRC_LFO, 1.0f);
    tessera_mod_eval(&m);
    CHECK(tessera_mod_value(&m, DST_CUTOFF) == 2000.0f, "depth 0: base value");

    tessera_mod_unroute(&m, r);
    r = tessera_mod_route(&m, SRC_LFO, DST_CUTOFF, 3000.0f, TESSERA_MOD_LIN);
    tessera_mod_eval(&m);
    CHECK(fabsf(tessera_mod_value(&m, DST_CUTOFF) - 5000.0f) < 1e-3f,
          "disabled slot reused by the next route");
    tessera_mod_unroute(&m, r);
    tessera_mod_eval(&m);
    CHECK(tessera_mod_value(&m, DST_CUTOFF) == 2000.0f,
          "removed route: back to the base value");

    /* Untouched destinations always evaluate to their base. */
    CHECK(tessera_mod_value(&m, DST_AMP) == 0.8f, "unrouted destination at base");
}

static void test_curves(void)
{
    printf("- curves shape the source before depth\n");
    tessera_mod_t m;
    fresh(&m);
    int r = tessera_mod_route(&m, SRC_LFO, DST_PITCH, 12.0f, TESSERA_MOD_EXP);
    tessera_mod_set_source(&m, SRC_LFO, 0.5f);
    tessera_mod_eval(&m);
    CHECK(fabsf(tessera_mod_value(&m, DST_PITCH) - 3.0f) < 1e-4f,
          "EXP: 0.5 -> 0.25, x12 = +3 semitones");
    tessera_mod_set_source(&m, SRC_LFO, -0.5f);
    tessera_mod_eval(&m);
    CHECK(fabsf(tessera_mod_value(&m, DST_PITCH) + 3.0f) < 1e-4f,
          "EXP preserves sign: -0.5 -> -3");
    tessera_mod_unroute(&m, r);

    tessera_mod_route(&m, SRC_PEDAL, DST_AMP, 0.5f, TESSERA_MOD_INV);
    tessera_mod_set_source(&m, SRC_PEDAL, 1.0f);
    tessera_mod_eval(&m);
    CHECK(fabsf(tessera_mod_value(&m, DST_AMP) - 0.3f) < 1e-4f,
          "INV: pedal up takes 0.5 off the base 0.8");
}

static void test_guards(void)
{
    printf("- guards\n");
    tessera_mod_t m;
    fresh(&m);
    CHECK(tessera_mod_route(&m, NSRC, DST_CUTOFF, 1.0f, TESSERA_MOD_LIN) == -1,
          "out-of-range source refused");
    CHECK(tessera_mod_route(&m, SRC_LFO, NDST, 1.0f, TESSERA_MOD_LIN) == -1,
          "out-of-range destination refused");
    CHECK(tessera_mod_route(&m, SRC_LFO, DST_CUTOFF, 1.0f, 9) == -1,
          "unknown curve refused");
    int n = 0;
    for (uint32_t i = 0; i < MAXR + 4u; i++)
        if (tessera_mod_route(&m, SRC_LFO, DST_CUTOFF, 1.0f, TESSERA_MOD_LIN) >= 0)
            n++;
    CHECK(n == (int)MAXR, "table full refused past max_routes");
}

int main(void)
{
    printf("=== modulation matrix host tests (issue #188) ===\n");
    test_lfo_sweep();
    test_sum_and_clamp();
    test_zero_and_removed();
    test_curves();
    test_guards();

    if (g_fail) {
        printf("MODMATRIX TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("MODMATRIX TESTS: ALL PASS\n");
    return 0;
}
