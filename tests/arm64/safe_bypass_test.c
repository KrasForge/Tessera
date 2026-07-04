/* tests/arm64/safe_bypass_test.c - host unit tests for safe-mode bypass
 * (Theme A: reliability).
 *
 * The resolve logic decides, per block, whether the DAC-bound signal comes from
 * the live effect or, once the effect has died, from a dry passthrough of its
 * input - the never-go-silent guarantee.  It is pure, so it is checked here for
 * exact behaviour: live -> output, dead -> input, dead-with-no-upstream ->
 * silence, the latch, and the block counters.
 *
 * Build/run via:  make test-arm-safe-bypass
 */

#include "safe_bypass.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define N 8u

static int words_eq(const uint32_t *a, const uint32_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}
static int words_zero(const uint32_t *a, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != 0u) return 0;
    return 1;
}
static void fill(uint32_t *a, uint32_t base, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) a[i] = base + i;
}

static void test_live_uses_output(void)
{
    printf("- live effect: output reaches the DAC\n");
    sb_state_t s; sb_init(&s);
    uint32_t out[N], in[N], dst[N];
    fill(out, 0x1000, N); fill(in, 0x2000, N);
    int bypassed = sb_resolve(&s, 1, out, in, dst, N);
    CHECK(bypassed == 0, "reports not-bypassed while alive");
    CHECK(words_eq(dst, out, N), "dst == effect output");
    CHECK(s.normal_blocks == 1 && s.bypass_blocks == 0, "counts a normal block");
    CHECK(s.bypassed == 0, "not latched into bypass");
}

static void test_dead_bypasses_to_input(void)
{
    printf("- dead effect: input is passed through dry (never silent)\n");
    sb_state_t s; sb_init(&s);
    uint32_t out[N], in[N], dst[N];
    fill(out, 0x1000, N); fill(in, 0x2000, N);
    int bypassed = sb_resolve(&s, 0, out, in, dst, N);
    CHECK(bypassed == 1, "reports bypassed while dead");
    CHECK(words_eq(dst, in, N), "dst == effect input (dry), not output");
    CHECK(!words_eq(dst, out, N), "dst is not the (stale) effect output");
    CHECK(s.bypass_blocks == 1 && s.normal_blocks == 0, "counts a bypass block");
    CHECK(s.bypassed == 1, "latched into bypass");
}

static void test_dead_no_upstream_is_silent(void)
{
    printf("- dead source (no upstream): output is silenced, not garbage\n");
    sb_state_t s; sb_init(&s);
    uint32_t out[N], dst[N];
    fill(out, 0x1000, N);
    int bypassed = sb_resolve(&s, 0, out, (const uint32_t *)0, dst, N);
    CHECK(bypassed == 1, "reports bypassed");
    CHECK(words_zero(dst, N), "dst is all-zero (silence), never stale output");
}

static void test_latch_survives_transient_alive(void)
{
    printf("- latch: once dead, stays bypassed even if 'alive' flaps back\n");
    sb_state_t s; sb_init(&s);
    uint32_t out[N], in[N], dst[N];
    fill(out, 0x1000, N); fill(in, 0x2000, N);
    sb_resolve(&s, 0, out, in, dst, N);        /* dies */
    int bypassed = sb_resolve(&s, 1, out, in, dst, N);  /* 'alive' again */
    CHECK(bypassed == 1, "still bypassed after a spurious alive");
    CHECK(words_eq(dst, in, N), "still dry passthrough, not the effect output");
    CHECK(s.bypass_blocks == 2, "both blocks counted as bypass");
    CHECK(s.normal_blocks == 0, "no block ever counted as normal");
}

static void test_transition_mid_stream(void)
{
    printf("- transition: normal blocks then bypass blocks\n");
    sb_state_t s; sb_init(&s);
    uint32_t out[N], in[N], dst[N];
    fill(out, 0x1000, N); fill(in, 0x2000, N);
    for (int b = 0; b < 3; b++) sb_resolve(&s, 1, out, in, dst, N);   /* live */
    for (int b = 0; b < 4; b++) sb_resolve(&s, 0, out, in, dst, N);   /* dead */
    CHECK(s.normal_blocks == 3, "3 live blocks");
    CHECK(s.bypass_blocks == 4, "4 bypassed blocks");
    CHECK(words_eq(dst, in, N), "final block is the dry input");
}

int main(void)
{
    printf("=== Tessera safe-mode bypass tests (Theme A) ===\n");
    test_live_uses_output();
    test_dead_bypasses_to_input();
    test_dead_no_upstream_is_silent();
    test_latch_survives_transient_alive();
    test_transition_mid_stream();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
