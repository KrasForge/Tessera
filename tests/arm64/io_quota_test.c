/* tests/arm64/io_quota_test.c - host unit tests for the per-plugin syscall /
 * I/O-rate quota policy (Theme M22, issue #198).
 *
 * The accounting and escalation are pure C: within-window throttling at the
 * ceiling, the throttle/kill escalation across windows (mirroring the CPU
 * budget's mute/kill), forgiveness on a clean window, the killed latch, and
 * the byte-count (I/O-bandwidth) usage of the same primitive.  All validated
 * here under ASan + UBSan.  A small gate simulation ties it together: a plugin
 * hammering the syscall gate is throttled within the window and killed after
 * sustained abuse, while a well-behaved plugin under the ceiling is untouched.
 *
 * Build/run via:  make test-arm-iobudget
 */

#include "io_quota.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* ---- charging against the ceiling --------------------------------------- */

static void test_charge(void)
{
    printf("- charge: allow within the ceiling, refuse over it\n");
    io_quota_t q;
    ioq_init(&q, 4, 3);

    CHECK(ioq_charge(&q, 1) == 1, "1/4 charged");
    CHECK(ioq_charge(&q, 1) == 1, "2/4 charged");
    CHECK(ioq_charge(&q, 1) == 1, "3/4 charged");
    CHECK(ioq_charge(&q, 1) == 1, "4/4 charged (ceiling reached)");
    CHECK(ioq_charge(&q, 1) == 0, "5th refused (over ceiling)");
    CHECK(q.used == 4 && q.peak == 4, "used pinned at the ceiling, peak tracked");
    CHECK(q.throttled == 1 && q.hit == 1, "one unit throttled, offence flag armed");
    CHECK(!q.killed, "a single over-ceiling window does not kill on its own");
}

static void test_unlimited(void)
{
    printf("- unlimited: ceiling 0 never throttles\n");
    io_quota_t q;
    ioq_init(&q, 0, 3);
    for (int i = 0; i < 100000; i++)
        if (!ioq_charge(&q, 1)) { CHECK(0, "unlimited charge refused"); return; }
    CHECK(q.throttled == 0 && q.hit == 0, "nothing throttled, no offence");
    CHECK(ioq_window(&q) == IOQ_OK, "unlimited window is always OK");
}

/* ---- escalation across windows ------------------------------------------ */

static void test_escalation(void)
{
    printf("- escalation: throttle, throttle, kill at N consecutive\n");
    io_quota_t q;
    ioq_init(&q, 2, 3);

    /* window 1: overrun */
    ioq_charge(&q, 2); CHECK(ioq_charge(&q, 1) == 0, "w1 over ceiling");
    CHECK(ioq_window(&q) == IOQ_THROTTLE, "first offence throttles");
    CHECK(q.used == 0 && q.hit == 0, "window counters reset");

    /* window 2: overrun */
    ioq_charge(&q, 2); ioq_charge(&q, 1);
    CHECK(ioq_window(&q) == IOQ_THROTTLE, "second offence throttles");

    /* window 3: overrun -> kill */
    ioq_charge(&q, 2); ioq_charge(&q, 1);
    CHECK(ioq_window(&q) == IOQ_KILL, "third consecutive kills");
    CHECK(q.killed == 1 && q.offences == 3, "killed latched, offences counted");
    CHECK(ioq_window(&q) == IOQ_KILL, "dead plugins stay dead");
    CHECK(q.offences == 3, "no accounting after death");
    CHECK(ioq_charge(&q, 1) == 0, "a killed plugin is charged nothing");
}

static void test_forgiveness(void)
{
    printf("- forgiveness: a clean window resets the streak\n");
    io_quota_t q;
    ioq_init(&q, 2, 3);

    ioq_charge(&q, 3); CHECK(ioq_window(&q) == IOQ_THROTTLE, "offence 1");
    ioq_charge(&q, 3); CHECK(ioq_window(&q) == IOQ_THROTTLE, "offence 2");
    ioq_charge(&q, 2); CHECK(ioq_window(&q) == IOQ_OK, "clean window forgiven");
    CHECK(q.streak == 0 && q.offences == 2 && !q.killed,
          "streak reset, total kept, alive");
    ioq_charge(&q, 3); CHECK(ioq_window(&q) == IOQ_THROTTLE, "new streak starts at throttle");
    ioq_charge(&q, 3); CHECK(ioq_window(&q) == IOQ_THROTTLE, "still below threshold");
    ioq_charge(&q, 3); CHECK(ioq_window(&q) == IOQ_KILL, "three in a row kills");
    CHECK(q.offences == 5, "five offences in total");
}

static void test_kill_after_one(void)
{
    printf("- zero tolerance: kill_after=1 (and 0 clamps to 1)\n");
    io_quota_t q;
    ioq_init(&q, 2, 1);
    ioq_charge(&q, 3);
    CHECK(ioq_window(&q) == IOQ_KILL, "first offence kills");

    ioq_init(&q, 2, 0);
    CHECK(q.kill_after == 1, "kill_after=0 clamps to 1");
}

/* ---- byte-count (I/O-bandwidth) usage of the same primitive -------------- */

static void test_io_bandwidth(void)
{
    printf("- I/O bandwidth: the same quota with a per-window byte ceiling\n");
    io_quota_t q;
    ioq_init(&q, 4096, 2);                 /* 4 KiB per window */

    CHECK(ioq_charge(&q, 1024) == 1, "1 KiB write fits");
    CHECK(ioq_charge(&q, 3072) == 1, "3 KiB write fits (ceiling reached)");
    CHECK(ioq_charge(&q, 1) == 0, "one more byte refused");
    CHECK(ioq_charge(&q, 2048) == 0, "large write refused all-or-nothing (charges 0)");
    CHECK(q.used == 4096, "no partial charge past the ceiling");
    CHECK(q.throttled == 2049, "refused bytes accumulate");
    CHECK(ioq_window(&q) == IOQ_THROTTLE, "over-bandwidth window is an offence");
}

/* ---- gate simulation: throttle within a window, kill across windows ------ */

/* A plugin that issues `n` syscalls per block; the "gate" charges each and the
 * plugin observes how many were serviced. */
static int drive_block(io_quota_t *q, int n, int *serviced)
{
    int ok = 0;
    for (int i = 0; i < n; i++)
        if (ioq_charge(q, 1)) ok++;
    if (serviced) *serviced = ok;
    return ioq_window(q);
}

static void test_gate_sim(void)
{
    printf("- gate sim: a syscall-storm plugin is throttled then killed\n");
    io_quota_t hog;
    ioq_init(&hog, 8, 3);                   /* ceiling 8 syscalls/block */

    int serviced;
    int v1 = drive_block(&hog, 40, &serviced);
    CHECK(serviced == 8, "storm block: only the ceiling's worth is serviced");
    CHECK(v1 == IOQ_THROTTLE, "storm block is an offence (throttle)");
    int v2 = drive_block(&hog, 40, &serviced);
    int v3 = drive_block(&hog, 40, &serviced);
    CHECK(v2 == IOQ_THROTTLE && v3 == IOQ_KILL, "sustained storm is killed");
    CHECK(hog.killed, "abuser latched dead");

    printf("- gate sim: a well-behaved plugin under the ceiling is untouched\n");
    io_quota_t good;
    ioq_init(&good, 8, 3);
    int clean = 1;
    for (int b = 0; b < 1000; b++) {
        int v = drive_block(&good, 3, &serviced);   /* 3 << 8 every block */
        if (v != IOQ_OK || serviced != 3) clean = 0;
    }
    CHECK(clean, "1000 blocks under the ceiling: always OK, always fully serviced");
    CHECK(good.offences == 0 && good.throttled == 0 && !good.killed,
          "no false positives: zero offences, nothing throttled, alive");
}

/* ---- rendering ----------------------------------------------------------- */

static void test_render(void)
{
    printf("- render: the shell/profiler line\n");
    io_quota_t q;
    ioq_init(&q, 64, 3);
    ioq_charge(&q, 12);
    ioq_charge(&q, 200);                    /* refused: 200 throttled */
    char buf[128];
    int n = ioq_render(&q, 3, buf, sizeof buf);
    printf("    \"%s\"\n", buf);
    CHECK(n == (int)strlen(buf), "returned length matches the string");
    CHECK(strcmp(buf, "ioq: pid=3 used=12/64 peak=12 throttled=200 offences=0 killed=0") == 0,
          "line matches the expected format");

    char tiny[8];
    int m = ioq_render(&q, 3, tiny, sizeof tiny);
    CHECK(m <= 7 && tiny[m] == '\0', "truncation stays within the buffer, NUL-terminated");
}

int main(void)
{
    printf("=== syscall / I/O-rate quota host tests (issue #198) ===\n");
    test_charge();
    test_unlimited();
    test_escalation();
    test_forgiveness();
    test_kill_after_one();
    test_io_bandwidth();
    test_gate_sim();
    test_render();

    if (g_fail) {
        printf("IO-QUOTA TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("IO-QUOTA TESTS: ALL PASS\n");
    return 0;
}
