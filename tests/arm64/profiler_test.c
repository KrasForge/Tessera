/* tests/arm64/profiler_test.c - host unit tests for the per-plugin profiler
 * (Theme G, issue #129).
 *
 * Build/run via:  make test-arm-profiler
 */

#include "profiler.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static void test_cyc_to_us(void)
{
    printf("- cycles -> microseconds conversion\n");
    CHECK(prof_cyc_to_us(500, 1000000u) == 500u, "1 MHz timer: cycles == us");
    CHECK(prof_cyc_to_us(54000, 54000000u) == 1000u, "54 MHz timer: 54000 cyc == 1000 us");
    CHECK(prof_cyc_to_us(0, 54000000u) == 0u, "zero cycles -> 0 us");
    CHECK(prof_cyc_to_us(500, 0) == 0u, "zero cntfrq is safe");
}

static void test_build_and_headroom(void)
{
    printf("- build per-plugin rows and total load / headroom\n");
    /* 1 MHz timer so cycles == microseconds; 1000 us block period. */
    pt_entry_t in[3] = {
        { .tag = 3, .runs = 100, .overruns = 2, .min = 400, .max = 600, .sum = 50000 }, /* mean 500us */
        { .tag = 7, .runs = 200, .overruns = 0, .min = 150, .max = 250, .sum = 40000 }, /* mean 200us */
        { .tag = 9, .runs = 0,   .overruns = 0, .min = 0,   .max = 0,   .sum = 0 },      /* never ran */
    };
    prof_row_t out[8]; uint32_t total = 0;
    int n = prof_build(in, 3, 1000u, 1000000u, out, 8, &total);
    CHECK(n == 2, "the never-run plugin is skipped");
    CHECK(out[0].pid == 3 && out[0].mean_us == 500 && out[0].max_us == 600, "plugin 3: mean 500us, max 600us");
    CHECK(out[0].load_permille == 500, "plugin 3: 500us / 1000us block = 50.0% load");
    CHECK(out[0].overruns == 2, "overruns passed through");
    CHECK(out[1].pid == 7 && out[1].load_permille == 200, "plugin 7: 20.0% load");
    CHECK(total == 700, "total load = 70.0%");
    CHECK(prof_headroom(total) == 300, "headroom = 30.0%");
    CHECK(prof_headroom(1200) == 0, "overloaded graph -> 0 headroom (floored)");
}

static void test_render(void)
{
    printf("- render a shell prof line\n");
    prof_row_t r = { .pid = 3, .runs = 2400, .mean_us = 334, .max_us = 352,
                     .load_permille = 334, .overruns = 2 };
    char buf[96];
    prof_render(&r, buf, sizeof buf);
    printf("    %s\n", buf);
    CHECK(!strcmp(buf, "prof: pid=3 runs=2400 mean=334us max=352us load=33.4% overruns=2"),
          "formatted line matches the expected layout");
}

int main(void)
{
    printf("=== Tessera per-plugin profiler tests (Theme G, #129) ===\n");
    test_cyc_to_us();
    test_build_and_headroom();
    test_render();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
