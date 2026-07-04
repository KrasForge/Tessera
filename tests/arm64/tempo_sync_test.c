/* tests/arm64/tempo_sync_test.c - host unit tests for tempo-synced note values
 * and tap tempo (Theme C, issue #115).
 *
 * Build/run via:  make test-arm-tempo-sync
 */

#include "tempo_sync.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define SR 48000u

static void test_sync_samples(void)
{
    printf("- note values resolve to the right length in samples @120 BPM\n");
    uint32_t t = 120000u;
    CHECK(tempo_sync_samples(t, SR, TS_QUARTER)   == 24000u, "quarter = 24000 (0.5 s)");
    CHECK(tempo_sync_samples(t, SR, TS_EIGHTH)    == 12000u, "eighth = 12000");
    CHECK(tempo_sync_samples(t, SR, TS_SIXTEENTH) ==  6000u, "sixteenth = 6000");
    CHECK(tempo_sync_samples(t, SR, TS_HALF)      == 48000u, "half = 48000 (1 s)");
    CHECK(tempo_sync_samples(t, SR, TS_WHOLE)     == 96000u, "whole = 96000 (2 s)");
    CHECK(tempo_sync_samples(t, SR, TS_DOT_EIGHTH)  == 18000u, "dotted eighth = 18000");
    CHECK(tempo_sync_samples(t, SR, TS_TRIP_QUARTER) == 16000u, "quarter triplet = 16000");
}

static void test_sync_ms(void)
{
    printf("- note values resolve to milliseconds; tempo change retimes them\n");
    CHECK(tempo_sync_ms(120000u, TS_QUARTER) == 500u, "quarter @120 = 500 ms");
    CHECK(tempo_sync_ms(120000u, TS_EIGHTH)  == 250u, "eighth @120 = 250 ms");
    CHECK(tempo_sync_ms(120000u, TS_DOT_EIGHTH) == 375u, "dotted eighth @120 = 375 ms");
    /* a tempo change retimes the same note value with no discontinuity in code */
    CHECK(tempo_sync_ms(60000u,  TS_QUARTER) == 1000u, "quarter @60 = 1000 ms");
    CHECK(tempo_sync_ms(128000u, TS_QUARTER) == 469u,  "quarter @128 = 469 ms (rounded)");
}

static void test_tap_converges(void)
{
    printf("- tap tempo converges to a steady BPM\n");
    taptempo_t tt; taptempo_init(&tt);
    for (int i = 0; i < 5; i++) taptempo_tap(&tt, 24000u);   /* steady 0.5 s taps */
    CHECK(taptempo_mbpm(&tt, SR) == 120000u, "steady 0.5 s taps -> 120.000 BPM");

    taptempo_init(&tt);
    for (int i = 0; i < 5; i++) taptempo_tap(&tt, 20000u);   /* 144 BPM */
    CHECK(taptempo_mbpm(&tt, SR) == 144000u, "steady taps -> 144.000 BPM");
}

static void test_tap_outlier(void)
{
    printf("- a single mis-tap is rejected without lurching the tempo\n");
    taptempo_t tt; taptempo_init(&tt);
    for (int i = 0; i < 4; i++) taptempo_tap(&tt, 24000u);
    uint32_t before = taptempo_mbpm(&tt, SR);
    int accepted = taptempo_tap(&tt, 60000u);               /* wild outlier */
    CHECK(accepted == 0, "the outlier tap is rejected");
    CHECK(taptempo_mbpm(&tt, SR) == before, "the estimate is unchanged (120 BPM)");
    /* a normal tap after the mis-tap still works */
    taptempo_tap(&tt, 24000u);
    CHECK(taptempo_mbpm(&tt, SR) == 120000u, "recovers on the next good tap");
}

static void test_tap_new_tempo(void)
{
    printf("- two consistent taps at a new tempo adopt it\n");
    taptempo_t tt; taptempo_init(&tt);
    for (int i = 0; i < 4; i++) taptempo_tap(&tt, 24000u);   /* 120 BPM */
    /* switch to ~57.6 BPM (50000-frame taps, > 2x away): first is held as an
     * outlier, the second consistent one adopts the new tempo. */
    CHECK(taptempo_tap(&tt, 50000u) == 0, "first new-tempo tap is held (rejected)");
    CHECK(taptempo_tap(&tt, 50000u) == 1, "second consistent tap adopts the new tempo");
    CHECK(taptempo_mbpm(&tt, SR) == 57600u, "estimate is now ~57.6 BPM");
}

int main(void)
{
    printf("=== Tessera tempo-sync + tap-tempo tests (Theme C, #115) ===\n");
    test_sync_samples();
    test_sync_ms();
    test_tap_converges();
    test_tap_outlier();
    test_tap_new_tempo();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
