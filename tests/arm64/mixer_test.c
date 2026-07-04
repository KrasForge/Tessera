/* tests/arm64/mixer_test.c - host unit tests for the mixer / routing primitives
 * (Theme D, issue #118).
 *
 * Build/run via:  make test-arm-mixer
 */

#include "mixer.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define N 8u
static void fill(int16_t *a, int16_t v) { for (uint32_t i = 0; i < N; i++) a[i] = v; }
static int all(const int16_t *a, int16_t v) { for (uint32_t i = 0; i < N; i++) if (a[i] != v) return 0; return 1; }
static int eq(const int16_t *a, const int16_t *b) { for (uint32_t i = 0; i < N; i++) if (a[i] != b[i]) return 0; return 1; }

static void test_gain(void)
{
    printf("- gain: unity, half, and boost saturation\n");
    int16_t s[N], d[N];
    fill(s, 10000);
    mix_gain(d, s, MIX_ONE, N);       CHECK(all(d, 10000), "unity gain is identity");
    mix_gain(d, s, MIX_ONE / 2, N);   CHECK(all(d, 5000),  "half gain halves the signal");
    fill(s, 30000);
    mix_gain(d, s, 2 * MIX_ONE, N);   CHECK(all(d, 32767), "2x boost saturates at +full-scale");
}

static void test_add(void)
{
    printf("- add: sums sources into a bus, saturating\n");
    int16_t acc[N], s[N];
    fill(acc, 4000); fill(s, 6000);
    mix_add(acc, s, MIX_ONE, N);      CHECK(all(acc, 10000), "acc += full-gain source");
    fill(acc, 30000); fill(s, 30000);
    mix_add(acc, s, MIX_ONE, N);      CHECK(all(acc, 32767), "sum beyond full-scale saturates");
}

static void test_pan(void)
{
    printf("- pan: mono -> stereo, linear law\n");
    int16_t s[N], l[N], r[N];
    fill(s, 8000);
    mix_pan(s, MIX_ONE / 2, l, r, N); CHECK(all(l, 4000) && all(r, 4000), "centre: half to each side");
    mix_pan(s, 0, l, r, N);           CHECK(all(l, 8000) && all(r, 0),    "hard left: all L, no R");
    mix_pan(s, MIX_ONE, l, r, N);     CHECK(all(l, 0) && all(r, 8000),    "hard right: all R, no L");
}

static void test_blend(void)
{
    printf("- wet/dry blend and true bypass\n");
    int16_t dry[N], wet[N], d[N];
    fill(dry, 10000); fill(wet, 2000);
    mix_blend(d, dry, wet, 0, N);         CHECK(all(d, 10000), "mix=0 -> fully dry");
    mix_blend(d, dry, wet, MIX_ONE, N);   CHECK(all(d, 2000),  "mix=ONE -> fully wet");
    mix_blend(d, dry, wet, MIX_ONE / 2, N);
    CHECK(all(d, 6000), "mix=1/2 -> average of dry and wet");
    /* a constant survives an identical dry==wet blend (unity-sum gains) */
    fill(wet, 10000);
    mix_blend(d, dry, wet, 12345, N);     CHECK(all(d, 10000), "dry==wet blend is transparent");

    for (uint32_t i = 0; i < N; i++) dry[i] = (int16_t)(1000 * (int)i - 3000);
    mix_bypass(d, dry, N);                CHECK(eq(d, dry), "true bypass is bit-exact");
}

int main(void)
{
    printf("=== Tessera mixer / routing tests (Theme D, #118) ===\n");
    test_gain();
    test_add();
    test_pan();
    test_blend();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
