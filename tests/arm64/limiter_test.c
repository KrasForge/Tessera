/* tests/arm64/limiter_test.c - host unit tests for the master limiter / soft-clip
 * (Theme M15, issue #166).
 *
 * Build/run via:  make test-arm-limiter
 */

#include "limiter.h"

#include <stdio.h>
#include <stdint.h>
#include <math.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define LA 64          /* look-ahead */
#define CEIL 24000     /* ceiling amplitude */

static void test_brickwall_square(void)
{
    printf("- a full-scale square never exceeds the ceiling\n");
    int16_t dbuf[LA]; int32_t gbuf[LA];
    limiter_t l;
    limiter_init(&l, dbuf, gbuf, LA, CEIL, 8);

    int16_t peak = 0;
    for (int i = 0; i < 4000; i++) {
        int16_t x = (i / 20 % 2) ? 32767 : -32768;   /* full-scale square */
        int16_t y = limiter_process(&l, x);
        int16_t a = y < 0 ? (int16_t)(-y) : y;
        if (a > peak) peak = a;
    }
    CHECK(peak <= CEIL, "no output sample exceeds the ceiling");
    CHECK(peak > CEIL - 2000, "and it reaches close to the ceiling (not over-limited)");
}

static void test_brickwall_sine(void)
{
    printf("- a loud sine is held under the ceiling\n");
    int16_t dbuf[LA]; int32_t gbuf[LA];
    limiter_t l;
    limiter_init(&l, dbuf, gbuf, LA, CEIL, 4);

    int over = 0, peak = 0;
    for (int i = 0; i < 9600; i++) {
        double s = sin(2.0 * 3.14159265358979 * 997.0 * i / 48000.0);
        int16_t x = (int16_t)(30000.0 * s);          /* above the ceiling */
        int16_t y = limiter_process(&l, x);
        int a = y < 0 ? -y : y;
        if (a > CEIL) over++;
        if (a > peak) peak = a;
    }
    CHECK(over == 0, "no sample exceeds the ceiling across the run");
    CHECK(peak > CEIL - 3000, "gain reduction is not excessive");
}

static void test_transparent_below(void)
{
    printf("- a signal below the ceiling passes through (delayed, bit-exact)\n");
    int16_t dbuf[LA]; int32_t gbuf[LA];
    limiter_t l;
    limiter_init(&l, dbuf, gbuf, LA, CEIL, 8);

    int16_t in[512];
    for (int i = 0; i < 512; i++)
        in[i] = (int16_t)(10000.0 * sin(2.0 * 3.14159265 * 200.0 * i / 48000.0));

    int16_t out[512];
    for (int i = 0; i < 512; i++) out[i] = limiter_process(&l, in[i]);

    /* Output is the input delayed by the look-ahead, unchanged. */
    int ok = 1;
    for (int i = LA; i < 512; i++) if (out[i] != in[i - LA]) ok = 0;
    CHECK(ok, "below-ceiling signal is the input delayed by the look-ahead");
    CHECK(limiter_latency(&l) == LA, "reported latency == look-ahead");
}

static void test_softclip(void)
{
    printf("- soft-clip: transparent below the knee, bounded at the ceiling\n");
    /* Below half the ceiling: bit-exact. */
    CHECK(limiter_softclip(5000, CEIL) == 5000, "small signal is transparent");
    CHECK(limiter_softclip(-5000, CEIL) == -5000, "and symmetric");
    /* Above the knee but below the ceiling: compressed toward, never past. */
    int16_t y = limiter_softclip(30000, CEIL);
    CHECK(y < CEIL && y > CEIL - 4000, "a hot signal rounds up toward the ceiling");
    CHECK(limiter_softclip(32767, CEIL) <= CEIL, "full scale is bounded to the ceiling");
    CHECK(limiter_softclip(-32768, CEIL) >= -CEIL, "and on the negative side");
    /* Monotonic and continuous around the knee. */
    int16_t at_knee = limiter_softclip(CEIL / 2, CEIL);
    CHECK(at_knee == CEIL / 2, "exactly at the knee is still transparent");
}

int main(void)
{
    printf("=== Tessera master limiter / soft-clip tests (M15, #166) ===\n");
    test_brickwall_square();
    test_brickwall_sine();
    test_transparent_below();
    test_softclip();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
