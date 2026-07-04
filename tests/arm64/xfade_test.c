/* tests/arm64/xfade_test.c - host unit tests for glitch-free crossfade patch
 * switching (Theme A: reliability).
 *
 * The crossfade mixes the outgoing patch A and the incoming patch B with a
 * raised-cosine gain ramp so a patch swap moves the waveform continuously from
 * A to B instead of stepping - no click.  The logic is pure fixed-point, so it
 * is checked here for exact behaviour: the unit-gain invariant, exact
 * endpoints, monotonic bounded steps (the click-free property), identical-patch
 * transparency, the latch/switch bookkeeping, and full-scale overflow safety
 * (ASan/UBSan).
 *
 * Build/run via:  make test-arm-patch-switch
 */

#include "xfade.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define N 8u

static void fill_const(int16_t *a, int16_t v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) a[i] = v;
}
static int all_eq(const int16_t *a, int16_t v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (a[i] != v) return 0;
    return 1;
}
static int words_eq(const int16_t *a, const int16_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void test_gain_sum_invariant(void)
{
    printf("- gains sum to unity at every ramp step (Q15)\n");
    int ok = 1, mono = 1;
    uint32_t prev = 0;
    for (uint32_t k = 0; k <= XF_STEPS; k++) {
        uint32_t gB = xf_gain_b(k);
        uint32_t gA = (uint32_t)XF_ONE - gB;
        if (gA + gB != (uint32_t)XF_ONE) ok = 0;
        if (gB < prev) mono = 0;
        prev = gB;
    }
    CHECK(ok, "gA + gB == 32768 for steps 0..XF_STEPS");
    CHECK(mono, "B's gain is monotonic non-decreasing along the ramp");
    CHECK(xf_gain_b(0) == 0u, "step 0 is all-A (gB == 0)");
    CHECK(xf_gain_b(XF_STEPS) == (uint32_t)XF_ONE, "final step is all-B (gB == 32768)");
    CHECK(xf_gain_b(XF_STEPS + 5u) == (uint32_t)XF_ONE, "gain clamps past the end");
}

static void test_not_fading_copies_a(void)
{
    printf("- idle: the running patch A passes straight through\n");
    xf_state_t s; xf_init(&s);
    int16_t a[N], b[N], dst[N];
    fill_const(a, 5000, N); fill_const(b, -5000, N);
    int mixed = xf_block(&s, a, b, dst, N);
    CHECK(mixed == 0, "reports a straight copy while idle");
    CHECK(all_eq(dst, 5000, N), "dst == A, untouched by B");
    CHECK(s.steady_blocks == 1 && s.fade_blocks == 0, "counts a steady block");
    CHECK(!xf_active(&s), "still idle");
}

static void test_endpoints_exact(void)
{
    printf("- fade endpoints are exact: first block A, last block B\n");
    xf_state_t s; xf_init(&s);
    int16_t a[N], b[N], dst[N];
    fill_const(a, 12000, N); fill_const(b, 4000, N);
    xf_begin(&s);
    int mixed = xf_block(&s, a, b, dst, N);        /* step 0 */
    CHECK(mixed == 1, "reports a mix while fading");
    CHECK(words_eq(dst, a, N), "first fade block == A exactly");
    for (uint32_t k = 1; k < XF_STEPS; k++)
        xf_block(&s, a, b, dst, N);                /* steps 1..XF_STEPS-1 */
    CHECK(xf_active(&s), "still fading before the last step");
    xf_block(&s, a, b, dst, N);                    /* step XF_STEPS */
    CHECK(words_eq(dst, b, N), "last fade block == B exactly");
    CHECK(!xf_active(&s), "fade latched off after the last step");
    CHECK(s.switches == 1, "one crossfade completed");
    CHECK(s.fade_blocks == XF_STEPS + 1u, "counted XF_STEPS+1 mixed blocks");
}

static void test_monotonic_click_free(void)
{
    printf("- click-free: the mix ramps monotonically with small bounded steps\n");
    xf_state_t s; xf_init(&s);
    int16_t a[N], b[N], dst[N];
    const int16_t A = 12000, B = 4000;             /* an 8000-sample step if cut abruptly */
    fill_const(a, A, N); fill_const(b, B, N);
    xf_begin(&s);

    int prev = A, mono = 1, max_step = 0, endpoints_ok = 1;
    for (uint32_t k = 0; k <= XF_STEPS; k++) {
        xf_block(&s, a, b, dst, N);
        if (!all_eq(dst, dst[0], N)) endpoints_ok = 0;   /* uniform within a block */
        int v = dst[0];
        if (v > prev) mono = 0;                          /* A > B, so non-increasing */
        int d = prev - v; if (d < 0) d = -d;
        if (d > max_step) max_step = d;
        prev = v;
    }
    CHECK(endpoints_ok, "each mixed block is internally uniform (constant in => constant out)");
    CHECK(mono, "output moves monotonically from A toward B");
    CHECK(prev == B, "ramp lands exactly on B");
    printf("    max per-block step = %d (an abrupt cut would step %d)\n", max_step, A - B);
    CHECK(max_step < (A - B) / 8, "largest step is far below the abrupt-cut step (glitch-free)");
}

static void test_identical_patches_transparent(void)
{
    printf("- transparency: crossfading two identical patches is a bit-exact no-op\n");
    xf_state_t s; xf_init(&s);
    int16_t a[N], b[N], dst[N];
    /* a non-constant but identical A and B */
    for (uint32_t i = 0; i < N; i++) { a[i] = (int16_t)(1000 * (int)i - 3000); b[i] = a[i]; }
    xf_begin(&s);
    int exact = 1;
    for (uint32_t k = 0; k <= XF_STEPS; k++) {
        xf_block(&s, a, b, dst, N);
        if (!words_eq(dst, a, N)) exact = 0;
    }
    CHECK(exact, "every mixed block equals the (identical) input, bit-for-bit");
}

static void test_full_scale_no_overflow(void)
{
    printf("- full-scale swing: no overflow, exact endpoints (ASan/UBSan)\n");
    xf_state_t s; xf_init(&s);
    int16_t a[N], b[N], dst[N];
    fill_const(a, 32767, N); fill_const(b, -32768, N);   /* the widest possible step */
    xf_begin(&s);
    int16_t mid0 = 0;
    for (uint32_t k = 0; k <= XF_STEPS; k++) {
        xf_block(&s, a, b, dst, N);
        if (k == XF_STEPS / 2u) mid0 = dst[0];           /* gA == gB == 16384 */
    }
    /* (32767*16384 + -32768*16384) >> 15 == -16384 >> 15 == -1; the product
     * never leaves int32, which UBSan confirms across the whole ramp. */
    CHECK(mid0 == -1, "the equal-gain midpoint is exact (no overflow in the product)");
    CHECK(dst[0] == -32768, "ramp reaches the full-scale B endpoint exactly");
}

static void test_begin_idempotent(void)
{
    printf("- xf_begin during a fade does not restart it\n");
    xf_state_t s; xf_init(&s);
    int16_t a[N], b[N], dst[N];
    fill_const(a, 1000, N); fill_const(b, 2000, N);
    xf_begin(&s);
    xf_block(&s, a, b, dst, N);        /* step 0 -> now at step 1 */
    uint32_t step_before = s.step;
    xf_begin(&s);                      /* must be ignored */
    CHECK(s.step == step_before, "ramp position is preserved");
    CHECK(xf_active(&s), "still fading");
}

int main(void)
{
    printf("=== Tessera crossfade patch-switch tests (Theme A) ===\n");
    test_gain_sum_invariant();
    test_not_fading_copies_a();
    test_endpoints_exact();
    test_monotonic_click_free();
    test_identical_patches_transparent();
    test_full_scale_no_overflow();
    test_begin_idempotent();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
