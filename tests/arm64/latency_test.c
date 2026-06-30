/* tests/arm64/latency_test.c - host unit tests for the latency statistics
 * (Issue #22).
 *
 * The min/max/mean/stddev math, the integer square root, and the overflow-safe
 * cycles->microseconds conversion are pure, so the numbers the audio core will
 * report over UART are checked here for exact correctness.  Using a counter
 * frequency of 1 MHz makes one cycle equal one microsecond, so expected values
 * are easy to state.
 *
 * Build/run via:  make test-arm-latency
 */

#include "latency.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define FREQ_1MHZ 1000000ull   /* 1 cycle == 1 us */

static void test_isqrt(void)
{
    printf("- integer sqrt\n");
    CHECK(lat_isqrt(0) == 0, "isqrt(0)=0");
    CHECK(lat_isqrt(1) == 1, "isqrt(1)=1");
    CHECK(lat_isqrt(15) == 3, "isqrt(15)=3 (floor)");
    CHECK(lat_isqrt(16) == 4, "isqrt(16)=4");
    CHECK(lat_isqrt(1000000) == 1000, "isqrt(1e6)=1000");
}

static void test_cyc_to_us(void)
{
    printf("- cycles -> microseconds\n");
    CHECK(lat_cyc_to_us(1000000, 1000000) == 1000000, "1e6 cyc @1MHz = 1e6 us");
    CHECK(lat_cyc_to_us(62500, 62500000) == 1000, "62500 cyc @62.5MHz = 1000 us");
    CHECK(lat_cyc_to_us(31, 62500000) == 0, "sub-us rounds toward 0");
    /* No overflow for large cycle counts (~1e12 cycles). */
    CHECK(lat_cyc_to_us(1000000000000ull, 1000000) == 1000000000000ull * 1000000ull / 1000000ull,
          "large cycle count does not overflow");
}

static void test_period_stats(void)
{
    printf("- windowed period stats (min/max/mean/stddev)\n");
    lat_stats_t s;
    lat_init(&s, FREQ_1MHZ);

    /* Feed callbacks whose successive deltas are 10,20,30,40 us. */
    uint64_t now = 0;
    lat_record(&s, now);             /* seed: returns 0, no delta stored */
    uint64_t deltas[4] = {10, 20, 30, 40};
    for (int i = 0; i < 4; i++) {
        now += deltas[i];
        lat_record(&s, now);
    }

    lat_summary_t out;
    lat_summary(&s, &out);
    CHECK(out.count == 4, "four deltas recorded");
    CHECK(out.min_us == 10, "min = 10 us");
    CHECK(out.max_us == 40, "max = 40 us");
    CHECK(out.mean_us == 25, "mean = 25 us");
    /* variance = ((15^2+5^2+5^2+15^2)/4) = 125; stddev = floor(sqrt(125)) = 11 */
    CHECK(out.stddev_us == 11, "stddev = 11 us");
}

static void test_constant_period(void)
{
    printf("- perfectly periodic stream has zero jitter\n");
    lat_stats_t s;
    lat_init(&s, FREQ_1MHZ);
    uint64_t now = 12345;
    lat_record(&s, now);
    for (int i = 0; i < 500; i++) {
        now += 1000;                 /* exactly 1 ms apart */
        lat_record(&s, now);
    }
    lat_summary_t out;
    lat_summary(&s, &out);
    CHECK(out.min_us == 1000 && out.max_us == 1000, "min == max == 1000 us");
    CHECK(out.mean_us == 1000, "mean = 1000 us");
    CHECK(out.stddev_us == 0, "stddev = 0 (no jitter)");
}

static void test_window_eviction(void)
{
    printf("- ring keeps only the most recent %u callbacks\n", LAT_WINDOW);
    lat_stats_t s;
    lat_init(&s, FREQ_1MHZ);
    uint64_t now = 0;
    lat_record(&s, now);
    /* First 100 deltas of 5000 us, then LAT_WINDOW deltas of 1000 us.  The big
     * early ones must be evicted, leaving min==max==1000. */
    for (int i = 0; i < 100; i++)            { now += 5000; lat_record(&s, now); }
    for (uint32_t i = 0; i < LAT_WINDOW; i++) { now += 1000; lat_record(&s, now); }

    lat_summary_t out;
    lat_summary(&s, &out);
    CHECK(out.count == LAT_WINDOW, "window is capped at LAT_WINDOW");
    CHECK(out.max_us == 1000, "the old 5000 us spikes were evicted");
    CHECK(out.min_us == 1000, "only recent 1000 us deltas remain");
}

static void test_wakeup(void)
{
    printf("- IRQ-to-thread wakeup metric\n");
    lat_stats_t s;
    lat_init(&s, FREQ_1MHZ);
    lat_record_wakeup(&s, 5);
    lat_record_wakeup(&s, 15);
    lat_record_wakeup(&s, 10);
    lat_summary_t out;
    lat_summary(&s, &out);
    CHECK(out.wake_max_us == 15, "wakeup max = 15 us");
    CHECK(out.wake_mean_us == 10, "wakeup mean = 10 us");
}

int main(void)
{
    printf("=== Tessera audio-latency stats tests (issue #22) ===\n");
    test_isqrt();
    test_cyc_to_us();
    test_period_stats();
    test_constant_period();
    test_window_eviction();
    test_wakeup();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
