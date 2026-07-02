/* tests/arm64/plugin_time_test.c - host unit tests for per-plugin time
 * accounting (Issue #77).
 *
 * The measurement (worker clock around each node run), the accumulation
 * (per-node min/max/sum since assignment), the seqlocked board handoff, and
 * the stats-line rendering are all pure C, so the numbers are validated on
 * the host against hand-computed values - the same approach as
 * tests/arm64/latency_test.c - plus a real two-thread publisher/reader
 * stress on the seqlock.
 *
 * Build/run via:  make test-arm-ptime
 */

#include "plugin_time.h"
#include "latency.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* ---- scripted fake clock: returns the next value from a sequence ---- */
#define CLK_MAX 64
static uint64_t g_clk_seq[CLK_MAX];
static int g_clk_at, g_clk_n;

static uint64_t fake_clock(void)
{
    return (g_clk_at < g_clk_n) ? g_clk_seq[g_clk_at++] : g_clk_seq[g_clk_n - 1];
}

static void clk_script(const uint64_t *vals, int n)
{
    for (int i = 0; i < n; i++)
        g_clk_seq[i] = vals[i];
    g_clk_at = 0;
    g_clk_n  = n;
}

static void nop_node(void *ctx) { (void)ctx; }

/* ---- accounting math vs hand-computed values ---- */
static void test_accounting(void)
{
    printf("- accounting: min/max/mean against hand-computed values\n");
    audio_worker_t w;
    aw_init(&w, 1);
    w.clock = fake_clock;
    int s0 = aw_assign(&w, nop_node, 0);
    int s1 = aw_assign(&w, nop_node, 0);
    w.nodes[s0].tag = 7;
    w.nodes[s1].tag = 9;

    /* Three blocks, two nodes each: node0 takes 100, 250, 100 cycles;
     * node1 takes 40, 40, 400.  (t0/t1 pairs, consecutive.) */
    const uint64_t script[] = {
        1000, 1100, 1100, 1140,          /* block 1: 100, 40  */
        2000, 2250, 2250, 2290,          /* block 2: 250, 40  */
        3000, 3100, 3100, 3500,          /* block 3: 100, 400 */
    };
    clk_script(script, 12);

    for (uint64_t seq = 1; seq <= 3; seq++) {
        aw_kick(&w, seq);
        aw_worker_step(&w);
    }

    CHECK(w.nodes[s0].runs == 3 && w.nodes[s1].runs == 3, "three runs each");
    CHECK(w.nodes[s0].svc_min == 100 && w.nodes[s0].svc_max == 250 &&
          w.nodes[s0].svc_sum == 450, "node0: min=100 max=250 sum=450");
    CHECK(w.nodes[s1].svc_min == 40 && w.nodes[s1].svc_max == 400 &&
          w.nodes[s1].svc_sum == 480, "node1: min=40 max=400 sum=480");

    /* Reassignment starts a fresh window. */
    aw_clear(&w);
    int s2 = aw_assign(&w, nop_node, 0);
    CHECK(w.nodes[s2].runs == 0 && w.nodes[s2].svc_sum == 0 &&
          w.nodes[s2].svc_max == 0, "assignment resets the window");
}

/* ---- accounting off: no clock, no cost, no numbers ---- */
static void test_disabled(void)
{
    printf("- disabled: without a clock nothing is measured\n");
    audio_worker_t w;
    aw_init(&w, 1);
    int s = aw_assign(&w, nop_node, 0);
    aw_kick(&w, 1);
    aw_worker_step(&w);
    CHECK(w.nodes[s].runs == 1, "the node still runs");
    CHECK(w.nodes[s].svc_sum == 0 && w.nodes[s].svc_max == 0,
          "no service time recorded");
}

/* ---- board publish/snapshot ---- */
static void test_board(void)
{
    printf("- board: publish, snapshot, torn-write detection\n");
    audio_worker_t w;
    aw_init(&w, 2);
    w.clock = fake_clock;
    int s0 = aw_assign(&w, nop_node, 0);
    w.nodes[s0].tag = 42;

    pt_board_t b;
    pt_board_init(&b);
    w.publish = pt_publish;
    w.pub_ctx = &b;

    const uint64_t script[] = { 10, 210 };   /* one 200-cycle run */
    clk_script(script, 2);
    aw_kick(&w, 1);
    aw_worker_step(&w);                       /* publishes the board */

    pt_entry_t snap[AW_MAX_NODES];
    int n = pt_snapshot(&b, snap, AW_MAX_NODES, 8);
    CHECK(n == 1, "snapshot returns the one node");
    CHECK(snap[0].tag == 42 && snap[0].runs == 1 &&
          snap[0].min == 200 && snap[0].max == 200 && snap[0].sum == 200,
          "published stats match the run");
    CHECK(snap[0].overruns == 0, "no overruns charged");

    /* A board frozen mid-publish (odd seq) must not yield a snapshot. */
    __atomic_store_n(&b.seq, b.seq + 1u, __ATOMIC_RELEASE);
    CHECK(pt_snapshot(&b, snap, AW_MAX_NODES, 8) == -1,
          "mid-publish board never settles");
    __atomic_store_n(&b.seq, b.seq + 1u, __ATOMIC_RELEASE);
    CHECK(pt_snapshot(&b, snap, AW_MAX_NODES, 8) == 1, "settles once even");

    /* Overrun attribution flows through to the board. */
    clk_script(script, 2);
    aw_kick(&w, 2);                           /* published, not yet run   */
    aw_kick(&w, 3);                           /* late: charged and skipped */
    aw_worker_step(&w);
    n = pt_snapshot(&b, snap, AW_MAX_NODES, 8);
    CHECK(n == 1 && snap[0].overruns == 1, "charged block reaches the board");
}

/* ---- rendering vs exact expected strings ---- */
static void test_render(void)
{
    printf("- render: the plugin_time line, cycles -> us\n");
    /* 62.5 MHz: 62500 cycles = 1000 us, 625 cycles = 10 us. */
    const uint64_t freq = 62500000ull;
    pt_entry_t e = { .tag = 7, .runs = 999, .overruns = 2,
                     .min = 625, .max = 62500, .sum = 999 * 1250 };
    char line[128];

    int n = pt_render(&e, "sine", freq, line, sizeof line);
    CHECK(n == (int)strlen(line), "returns the rendered length");
    CHECK(strcmp(line, "plugin_time: pid=7 (sine) runs=999 min=10us "
                       "max=1000us mean=20us overruns=2") == 0,
          "named line matches exactly");

    pt_render(&e, 0, freq, line, sizeof line);
    CHECK(strcmp(line, "plugin_time: pid=7 runs=999 min=10us "
                       "max=1000us mean=20us overruns=2") == 0,
          "anonymous line omits the name");

    pt_entry_t z = { .tag = 1, .runs = 0, .overruns = 0,
                     .min = 0, .max = 0, .sum = 0 };
    pt_render(&z, 0, freq, line, sizeof line);
    CHECK(strcmp(line, "plugin_time: pid=1 runs=0 min=0us "
                       "max=0us mean=0us overruns=0") == 0,
          "a never-run node renders zeros");

    /* Truncation stays NUL-terminated and in bounds. */
    char tiny[16];
    n = pt_render(&e, "sine", freq, tiny, sizeof tiny);
    CHECK(n == 15 && tiny[15] == '\0', "truncates safely");
}

/* ---- two-thread stress: worker publishes, reader snapshots ---- */
#define STRESS_BLOCKS 100000u

static audio_worker_t g_w;
static pt_board_t     g_b;
static volatile uint32_t g_stress_stop;
static uint64_t g_mono;

static uint64_t mono_clock(void) { return g_mono += 5; }  /* every run: 5 cyc */

static void *reader_thread(void *unused)
{
    (void)unused;
    pt_entry_t snap[AW_MAX_NODES];
    uint64_t reads = 0, bad = 0;
    while (!__atomic_load_n(&g_stress_stop, __ATOMIC_ACQUIRE)) {
        int n = pt_snapshot(&g_b, snap, AW_MAX_NODES, 1000);
        if (n < 0)
            continue;                          /* writer was busy: fine */
        reads++;
        /* Consistency invariants of any settled snapshot. */
        for (int i = 0; i < n; i++) {
            uint64_t mean = snap[i].runs ? snap[i].sum / snap[i].runs : 0;
            if (snap[i].tag != 42) bad++;
            if (snap[i].runs && (snap[i].min > mean || mean > snap[i].max))
                bad++;
            if (snap[i].sum != snap[i].runs * 5) bad++;   /* 5 cyc per run */
        }
    }
    return (void *)(uintptr_t)((reads > 0 && bad == 0) ? 1 : 0);
}

static void test_threads(void)
{
    printf("- two-thread stress: seqlock under concurrent publish/read\n");
    aw_init(&g_w, 1);
    g_w.clock   = mono_clock;
    g_w.publish = pt_publish;
    g_w.pub_ctx = &g_b;
    pt_board_init(&g_b);
    int s = aw_assign(&g_w, nop_node, 0);
    g_w.nodes[s].tag = 42;

    pthread_t t;
    pthread_create(&t, 0, reader_thread, 0);

    for (uint64_t seq = 1; seq <= STRESS_BLOCKS; seq++) {
        aw_kick(&g_w, seq);
        while (!aw_worker_step(&g_w))
            ;
    }
    __atomic_store_n(&g_stress_stop, 1u, __ATOMIC_RELEASE);
    void *r;
    pthread_join(t, &r);

    CHECK(g_w.nodes[s].runs == STRESS_BLOCKS, "every block ran and published");
    CHECK((uintptr_t)r == 1, "every settled snapshot was consistent");
}

int main(void)
{
    printf("=== plugin time accounting host tests (issue #77) ===\n");
    test_accounting();
    test_disabled();
    test_board();
    test_render();
    test_threads();

    if (g_fail) {
        printf("PTIME TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("PTIME TESTS: ALL PASS\n");
    return 0;
}
