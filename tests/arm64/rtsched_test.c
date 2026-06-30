/* tests/arm64/rtsched_test.c - host unit tests for the RT scheduler policy
 * (Issue #20).
 *
 * The scheduling decisions (priority ordering, round-robin fairness, time
 * slicing, and the priority-inheritance mutex) are pure C in runqueue.c, so
 * the acceptance criteria can be checked deterministically on the host:
 *   - a REALTIME task preempts a NORMAL one;
 *   - the IDLE task runs only when nothing else is runnable;
 *   - round-robin among equal-priority NORMAL tasks is fair over
 *     100 tasks x 1000 ticks (no starvation);
 *   - priority inheritance boosts a low-priority mutex holder.
 *
 * Build/run via:  make test-arm-rtsched
 */

#include "runqueue.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Drive the scheduler exactly as arch/arm64/sched.c does: pick a first task,
 * then on each tick ask the run queue for the successor and commit a switch
 * when it differs.  Returns the id running after the tick. */
static int sim_tick(void)
{
    int nx = rq_tick();
    if (nx != RQ_NONE && nx != rq_current())
        rq_commit(nx);
    return rq_current();
}

static void test_priority_preempts_normal(void)
{
    printf("- REALTIME preempts NORMAL\n");
    rq_init();
    rq_add(0, SCHED_PRIO_NORMAL,   5);
    rq_add(1, SCHED_PRIO_REALTIME, 5);

    /* Highest priority is picked first regardless of insertion order. */
    int first = rq_pick();
    rq_commit(first);
    CHECK(first == 1, "REALTIME task selected before NORMAL");

    /* While the RT task is runnable it keeps the CPU across ticks. */
    int held = 1;
    for (int i = 0; i < 20; i++)
        if (sim_tick() != 1) held = 0;
    CHECK(held, "REALTIME holds the CPU while runnable (NORMAL starved)");

    /* When the RT task leaves, the NORMAL task runs. */
    rq_remove(1);
    CHECK(sim_tick() == 0, "NORMAL runs once REALTIME exits");
}

static void test_preempt_midslice(void)
{
    printf("- higher priority preempts mid-slice\n");
    rq_init();
    rq_add(0, SCHED_PRIO_NORMAL, 10);
    int f = rq_pick();
    rq_commit(f);                       /* NORMAL running, 10-tick slice */
    sim_tick(); sim_tick();             /* 2 ticks into its slice        */

    /* A REALTIME task becomes runnable: it must preempt before the NORMAL
     * task's slice expires. */
    rq_add(1, SCHED_PRIO_REALTIME, 10);
    CHECK(sim_tick() == 1, "REALTIME preempts NORMAL mid-slice (no waiting for quantum)");
}

static void test_idle_only_when_empty(void)
{
    printf("- IDLE runs only when nothing else is runnable\n");
    rq_init();
    rq_add(0, SCHED_PRIO_NORMAL, 5);
    rq_add(1, SCHED_PRIO_IDLE,   5);
    int f = rq_pick();
    rq_commit(f);
    CHECK(f == 0, "NORMAL chosen over IDLE");

    int saw_idle = 0;
    for (int i = 0; i < 50; i++)
        if (sim_tick() == 1) saw_idle = 1;
    CHECK(!saw_idle, "IDLE never runs while a NORMAL task is runnable");

    /* Block the NORMAL task: now only IDLE is runnable. */
    rq_block(0);
    CHECK(sim_tick() == 1, "IDLE runs when the NORMAL task blocks");

    /* Unblock NORMAL: IDLE is immediately preempted. */
    rq_unblock(0);
    CHECK(sim_tick() == 0, "IDLE is preempted the moment NORMAL is runnable again");
}

static void test_round_robin_fairness(void)
{
    printf("- round-robin fairness: 100 tasks x 1000 ticks, no starvation\n");
    rq_init();
    enum { N = 100, TICKS_PER = 1000 };
    for (int i = 0; i < N; i++)
        rq_add(i, SCHED_PRIO_NORMAL, 5);

    int first = rq_pick();
    rq_commit(first);
    for (long t = 0; t < (long)N * TICKS_PER; t++)
        sim_tick();

    /* Every task must have run, and shares must be close to the mean (a fair
     * round-robin gives each task quantum/(N*quantum) = 1/N of the CPU). */
    uint64_t total = 0, lo = (uint64_t)-1, hi = 0;
    int all_ran = 1;
    for (int i = 0; i < N; i++) {
        uint64_t r = rq_ran_ticks(i);
        if (r == 0) all_ran = 0;
        if (r < lo) lo = r;
        if (r > hi) hi = r;
        total += r;
    }
    uint64_t mean = total / N;
    printf("    mean=%llu min=%llu max=%llu spread=%llu\n",
           (unsigned long long)mean, (unsigned long long)lo,
           (unsigned long long)hi, (unsigned long long)(hi - lo));

    CHECK(all_ran, "every one of the 100 tasks was scheduled (no starvation)");
    /* Round-robin with a fixed quantum is near-perfectly fair: the spread
     * between the busiest and idlest task is at most one quantum. */
    CHECK(hi - lo <= 5, "CPU shares are within one quantum of each other");
    CHECK(lo >= mean - mean / 10, "no task got less than 90% of the fair share");
}

static void test_priority_inheritance(void)
{
    printf("- priority inheritance mutex (anti-inversion)\n");
    rq_init();
    rq_add(0, SCHED_PRIO_NORMAL,   5);   /* low-priority holder    */
    rq_add(1, SCHED_PRIO_REALTIME, 5);   /* high-priority waiter   */

    rq_mutex_t m;
    rq_mutex_init(&m);

    CHECK(rq_mutex_lock(&m, 0) == 1, "low-priority task acquires the mutex");
    CHECK(rq_effective_prio(0) == SCHED_PRIO_NORMAL, "holder starts at NORMAL");

    /* RT task contends: it blocks, and the holder inherits RT priority. */
    CHECK(rq_mutex_lock(&m, 1) == 0, "REALTIME task blocks on the held mutex");
    CHECK(rq_effective_prio(0) == SCHED_PRIO_REALTIME,
          "holder inherits REALTIME priority (no inversion)");
    CHECK(rq_effective_prio(1) == SCHED_PRIO_REALTIME, "waiter keeps its priority");

    /* Holder releases: its priority drops back and the waiter gets the lock. */
    rq_mutex_unlock(&m, 0);
    CHECK(rq_effective_prio(0) == SCHED_PRIO_NORMAL, "holder restored to NORMAL on release");
    CHECK(m.holder == 1, "mutex handed to the REALTIME waiter");
    CHECK(rq_effective_prio(1) != RQ_NONE, "waiter is runnable again");
}

int main(void)
{
    printf("=== Tessera RT-scheduler policy tests (issue #20) ===\n");

    test_priority_preempts_normal();
    test_preempt_midslice();
    test_idle_only_when_empty();
    test_round_robin_fairness();
    test_priority_inheritance();

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
