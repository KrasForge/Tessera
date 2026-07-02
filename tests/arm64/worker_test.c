/* tests/arm64/worker_test.c - host unit tests for the per-core audio workers
 * (Issue #74).
 *
 * The kick/step protocol is pure C, so everything that must be right for the
 * audio core never to block on a worker is checked on the host: empty workers
 * are not kicked and park, assigned nodes run once per published block, a late
 * worker's kick is skipped and accounted against it and its nodes (never
 * queued), recovery after catching up is automatic, and the drained invariant
 * blocks + overruns == kicks holds - including under a real two-thread
 * kicker/worker pair exercising the cross-core acquire/release ordering.
 *
 * Build/run via:  make test-arm-worker
 */

#include "audio_worker.h"

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* A node that counts its runs. */
static void bump(void *ctx) { (*(uint64_t *)ctx)++; }

/* ---- empty worker: never kicked, always parks ---- */
static void test_empty(void)
{
    printf("- empty worker: no kick, no work\n");
    audio_worker_t w;
    aw_init(&w, 1);

    CHECK(aw_kick(&w, 1) == 1, "kick on empty worker is a no-op success");
    CHECK(w.kicks == 0 && w.overruns == 0, "nothing accounted");
    CHECK(aw_worker_step(&w) == 0, "step finds nothing and parks");
    CHECK(aw_drained(&w), "empty worker is drained");
}

/* ---- basic cadence: each kick runs each node exactly once ---- */
static void test_cadence(void)
{
    printf("- cadence: one kick, one block, every node once\n");
    audio_worker_t w;
    aw_init(&w, 1);

    uint64_t a = 0, b = 0;
    CHECK(aw_assign(&w, bump, &a) == 0, "first node lands in slot 0");
    CHECK(aw_assign(&w, bump, &b) == 1, "second node lands in slot 1");

    for (uint64_t seq = 1; seq <= 5; seq++) {
        CHECK(aw_kick(&w, seq) == 1, "kick published");
        CHECK(aw_worker_step(&w) == 1, "step runs the block");
    }
    CHECK(a == 5 && b == 5, "both nodes ran once per block");
    CHECK(w.nodes[0].runs == 5 && w.nodes[1].runs == 5, "per-node run counts");
    CHECK(w.blocks == 5 && w.kicks == 5 && w.overruns == 0, "worker counters");
    CHECK(aw_worker_step(&w) == 0, "no double-run of a completed block");
    CHECK(aw_drained(&w), "drained after answering every kick");
}

/* ---- late worker: kick skipped, accounted, then automatic recovery ---- */
static void test_late(void)
{
    printf("- late worker: skip + attribute, then recover\n");
    audio_worker_t w;
    aw_init(&w, 2);

    uint64_t a = 0;
    aw_assign(&w, bump, &a);

    CHECK(aw_kick(&w, 1) == 1, "block 1 published");
    /* The worker does not step: it is stalled.  Blocks 2 and 3 arrive. */
    CHECK(aw_kick(&w, 2) == 0, "block 2 skipped while late");
    CHECK(aw_kick(&w, 3) == 0, "block 3 skipped while late");
    CHECK(w.overruns == 2, "worker charged for both skipped blocks");
    CHECK(w.nodes[0].overruns == 2, "node charged for both skipped blocks");

    /* The stall ends: the worker answers block 1, and only block 1. */
    CHECK(aw_worker_step(&w) == 1, "stalled block finally executes");
    CHECK(aw_worker_step(&w) == 0, "skipped blocks were never queued");
    CHECK(a == 1 && w.blocks == 1, "exactly one block ran");

    /* Recovery: the very next kick succeeds. */
    CHECK(aw_kick(&w, 4) == 1, "kick succeeds after catch-up");
    CHECK(aw_worker_step(&w) == 1, "and the block runs");
    CHECK(w.blocks + w.overruns == w.kicks, "drained invariant holds");
    CHECK(aw_drained(&w), "worker drained");
}

/* ---- clear: unassigned worker parks again; counters attribute per set ---- */
static void test_clear(void)
{
    printf("- clear: plugin unload returns the worker to parked\n");
    audio_worker_t w;
    aw_init(&w, 3);

    uint64_t a = 0;
    aw_assign(&w, bump, &a);
    aw_kick(&w, 1);
    aw_worker_step(&w);

    aw_clear(&w);
    CHECK(aw_kick(&w, 2) == 1, "kick after clear is a no-op success");
    CHECK(aw_worker_step(&w) == 0, "nothing to run after clear");
    CHECK(w.kicks == 1 && w.blocks == 1, "counters stop with the assignment");
}

/* ---- two-thread stress: kicker vs worker, ordering and the invariant ---- */
#define STRESS_KICKS 200000u

static audio_worker_t g_w;
static volatile uint64_t g_ran;
static volatile uint32_t g_worker_stop;

static void stress_node(void *ctx)
{
    (void)ctx;
    g_ran++;                      /* only the worker thread writes this */
}

static void *worker_thread(void *unused)
{
    (void)unused;
    while (!__atomic_load_n(&g_worker_stop, __ATOMIC_ACQUIRE))
        aw_worker_step(&g_w);
    return 0;
}

static void test_threads(void)
{
    printf("- two-thread stress: kicker/worker protocol under contention\n");
    aw_init(&g_w, 1);
    aw_assign(&g_w, stress_node, 0);

    pthread_t t;
    pthread_create(&t, 0, worker_thread, 0);

    for (uint64_t seq = 1; seq <= STRESS_KICKS; seq++)
        aw_kick(&g_w, seq);

    /* Let the worker drain the last published block, then stop it. */
    while (!aw_drained(&g_w))
        ;
    __atomic_store_n(&g_worker_stop, 1u, __ATOMIC_RELEASE);
    pthread_join(t, 0);

    printf("  kicks=%llu published=%llu skipped=%llu\n",
           (unsigned long long)g_w.kicks,
           (unsigned long long)(g_w.kicks - g_w.overruns),
           (unsigned long long)g_w.overruns);
    CHECK(g_w.kicks == STRESS_KICKS, "every block attempt accounted");
    CHECK(g_w.blocks + g_w.overruns == g_w.kicks,
          "each kick either ran or was charged, never both, never lost");
    CHECK(g_ran == g_w.blocks, "node ran exactly once per executed block");
    CHECK(g_w.nodes[0].runs == g_w.blocks &&
          g_w.nodes[0].overruns == g_w.overruns,
          "per-node attribution matches the worker");
    CHECK(g_w.blocks > 0, "the worker actually ran");
}

int main(void)
{
    printf("=== audio worker host tests (issue #74) ===\n");
    test_empty();
    test_cadence();
    test_late();
    test_clear();
    test_threads();

    if (g_fail) {
        printf("WORKER TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("WORKER TESTS: ALL PASS\n");
    return 0;
}
