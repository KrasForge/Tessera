/* tests/arm64/looper_test.c - host unit tests for the multi-track looper
 * (Theme M17, issue #172).
 *
 * Build/run via:  make test-arm-looper
 */

#include "looper.h"

#include <stdio.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define CAP 2048
static int16_t t0[CAP], t1[CAP], t2[CAP], t3[CAP];
static int16_t *const TRACKS[4] = { t0, t1, t2, t3 };

static void test_record_play(void)
{
    printf("- record a layer, then loop it back (quantised)\n");
    looper_t l;
    looper_init(&l, TRACKS, 4, CAP, 100);          /* 100-sample grid */

    looper_record(&l);
    for (int i = 0; i < 240; i++) looper_process(&l, 1000);   /* 240 samples of DC */
    looper_stop(&l);
    CHECK(l.loop_len == 200, "240 samples snapped to 200 (nearest grid multiple)");
    CHECK(l.state == LOOP_PLAYING, "playing after stop");

    /* Play one loop and capture it. */
    int16_t out[256];
    for (uint32_t i = 0; i < l.loop_len; i++) out[i] = looper_process(&l, 0);
    CHECK(out[100] == 1000, "the loop body reproduces the recorded signal");
    CHECK(out[0] == 0 && out[l.loop_len - 1] == 0, "the loop edges are ramped (click-free seam)");

    /* Loops seamlessly: the next pass is identical. */
    int16_t out2[256];
    for (uint32_t i = 0; i < l.loop_len; i++) out2[i] = looper_process(&l, 0);
    CHECK(out2[100] == 1000 && out2[50] == out[50], "the loop repeats identically");
}

static void test_quantize(void)
{
    printf("- record length snaps to the grid\n");
    looper_t l;
    looper_init(&l, TRACKS, 4, CAP, 100);
    looper_record(&l);
    for (int i = 0; i < 260; i++) looper_process(&l, 500);
    looper_stop(&l);
    CHECK(l.loop_len == 300, "260 samples snapped up to 300");

    looper_init(&l, TRACKS, 4, CAP, 100);
    looper_record(&l);
    for (int i = 0; i < 130; i++) looper_process(&l, 500);
    looper_stop(&l);
    CHECK(l.loop_len == 100, "130 samples snapped down to 100");
}

static void test_overdub(void)
{
    printf("- overdub sums a second layer on top\n");
    looper_t l;
    looper_init(&l, TRACKS, 4, CAP, 100);
    looper_record(&l);
    for (int i = 0; i < 200; i++) looper_process(&l, 1000);
    looper_stop(&l);                               /* loop_len 200, layer 0 = 1000 */

    looper_record(&l);                             /* -> OVERDUB on track 1 */
    CHECK(l.state == LOOP_OVERDUB, "record from playing starts an overdub");
    for (uint32_t i = 0; i < l.loop_len; i++) looper_process(&l, 500);   /* add 500 */
    looper_stop(&l);
    CHECK(l.layers == 2, "two layers after the overdub");

    int16_t out[256];
    for (uint32_t i = 0; i < l.loop_len; i++) out[i] = looper_process(&l, 0);
    CHECK(out[100] == 1500, "the two layers sum (1000 + 500)");
}

static void test_saturation(void)
{
    printf("- overdub summing saturates rather than wrapping\n");
    looper_t l;
    looper_init(&l, TRACKS, 4, CAP, 100);
    looper_record(&l);
    for (int i = 0; i < 200; i++) looper_process(&l, 30000);
    looper_stop(&l);
    looper_record(&l);
    for (uint32_t i = 0; i < l.loop_len; i++) looper_process(&l, 30000);   /* 60000 > full scale */
    looper_stop(&l);
    int16_t out[256];
    for (uint32_t i = 0; i < l.loop_len; i++) out[i] = looper_process(&l, 0);
    CHECK(out[100] == 32767, "the summed layers saturate at full scale");
}

static void test_memory_bound(void)
{
    printf("- recording is bounded by the fixed track buffer\n");
    enum { SMALL = 128 };
    static int16_t s0[SMALL], s1[SMALL];
    int16_t *const st[2] = { s0, s1 };
    looper_t l;
    looper_init(&l, st, 2, SMALL, 1);              /* no quantise */

    looper_record(&l);
    for (int i = 0; i < 500; i++) looper_process(&l, 1234);   /* far more than SMALL */
    CHECK(l.state == LOOP_PLAYING, "recording auto-stops at the buffer bound");
    CHECK(l.loop_len == SMALL, "the loop can never exceed the track capacity");
}

static void test_clear(void)
{
    printf("- clear resets to empty passthrough\n");
    looper_t l;
    looper_init(&l, TRACKS, 4, CAP, 100);
    looper_record(&l);
    for (int i = 0; i < 200; i++) looper_process(&l, 1000);
    looper_stop(&l);
    looper_clear(&l);
    CHECK(l.state == LOOP_IDLE && l.loop_len == 0, "cleared to idle");
    CHECK(looper_process(&l, 777) == 777, "idle passes the input through");
}

int main(void)
{
    printf("=== Tessera looper tests (M17, #172) ===\n");
    test_record_play();
    test_quantize();
    test_overdub();
    test_saturation();
    test_memory_bound();
    test_clear();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
