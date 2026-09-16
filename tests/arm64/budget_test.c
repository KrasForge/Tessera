/* tests/arm64/budget_test.c - host unit tests for CPU-budget enforcement
 * policy (Issue #78).
 *
 * The escalation policy (mute first, kill after N consecutive offences,
 * forgiveness on a clean block), the fair-share default, and the
 * control-plane budget registry are pure C; the numbers and the truth table
 * are validated here.  The preemption machinery itself (budget timer, EL0
 * IRQ window, kernel_resume unwind) is exercised end to end on QEMU by
 * make test-arm-budget-qemu.
 *
 * Build/run via:  make test-arm-budget
 */

#include "budget.h"
#include "graph_control.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static void test_fair_share(void)
{
    printf("- fair share: the default budget\n");
    CHECK(budget_fair_share(62500, 3) == 20833, "block/3 for three nodes");
    CHECK(budget_fair_share(62500, 1) == 62500, "one node owns the block");
    CHECK(budget_fair_share(62500, 0) == 62500, "zero nodes treated as one");
    CHECK(budget_fair_share(2, 5) == 1, "never rounds down to zero");
}

static void test_escalation(void)
{
    printf("- escalation: mute, mute, kill at N consecutive\n");
    budget_t b;
    budget_init(&b, 1000, 3);

    CHECK(budget_account(&b, 0) == BUDGET_OK, "clean block is OK");
    CHECK(budget_account(&b, 1) == BUDGET_MUTE, "first offence mutes");
    CHECK(budget_account(&b, 1) == BUDGET_MUTE, "second offence mutes");
    CHECK(budget_account(&b, 1) == BUDGET_KILL, "third consecutive kills");
    CHECK(b.killed == 1 && b.offences == 3, "killed latched, offences counted");
    CHECK(budget_account(&b, 0) == BUDGET_KILL, "dead plugins stay dead");
    CHECK(b.offences == 3, "no accounting after death");
}

static void test_forgiveness(void)
{
    printf("- forgiveness: a clean block resets the streak\n");
    budget_t b;
    budget_init(&b, 1000, 3);

    CHECK(budget_account(&b, 1) == BUDGET_MUTE, "offence 1");
    CHECK(budget_account(&b, 1) == BUDGET_MUTE, "offence 2");
    CHECK(budget_account(&b, 0) == BUDGET_OK, "clean block forgiven");
    CHECK(b.streak == 0 && b.offences == 2 && !b.killed,
          "streak reset, total kept, alive");
    CHECK(budget_account(&b, 1) == BUDGET_MUTE, "new streak starts at mute");
    CHECK(budget_account(&b, 1) == BUDGET_MUTE, "still below the threshold");
    CHECK(budget_account(&b, 1) == BUDGET_KILL, "three in a row kills");
    CHECK(b.offences == 5, "five offences in total");
}

static void test_kill_after_one(void)
{
    printf("- zero tolerance: kill_after=1 (and 0 clamps to 1)\n");
    budget_t b;
    budget_init(&b, 1000, 1);
    CHECK(budget_account(&b, 1) == BUDGET_KILL, "first offence kills");

    budget_init(&b, 1000, 0);
    CHECK(b.kill_after == 1, "kill_after=0 clamps to 1");
}

static void *reg_ring_new(void *ctx) { (void)ctx; static int r; return &r; }

static void test_registry(void)
{
    printf("- control plane: per-plugin budgets via graph_control\n");
    gc_ring_ops_t ops = { reg_ring_new, 0, 0, 0, 0 };
    graph_control_t gc;
    gc_init(&gc, &ops);
    gc_add_plugin(&gc, 7);
    gc_add_plugin(&gc, 9);

    CHECK(gc_budget(&gc, 7) == 0, "no budget set: 0 (caller applies fair share)");
    CHECK(gc_set_budget(&gc, 7, 20000) == GC_OK, "set for pid 7");
    CHECK(gc_set_budget(&gc, 9, 30000) == GC_OK, "set for pid 9");
    CHECK(gc_budget(&gc, 7) == 20000 && gc_budget(&gc, 9) == 30000,
          "budgets read back per pid");
    CHECK(gc_set_budget(&gc, 7, 25000) == GC_OK && gc_budget(&gc, 7) == 25000,
          "update in place");
    CHECK(gc_set_budget(&gc, 7, 0) == GC_OK && gc_budget(&gc, 7) == 0,
          "cycles=0 clears back to the default");
    CHECK(gc_set_budget(&gc, 42, 1000) == GC_ENODEV,
          "unknown pid rejected");
    CHECK(gc_set_budget(&gc, 0, 1000) == GC_ENODEV, "pid 0 rejected");
    CHECK(gc_budget(&gc, 0) == 0, "cleared slot never becomes a PID-zero budget");
    gc_add_input(&gc);
    CHECK(gc_set_budget(&gc, GRAPH_INPUT_PID, 1000) == GC_ENODEV,
          "capture input is not a budgetable plugin");
    CHECK(gc_set_budget(&gc, 9, UINT64_MAX) == GC_EINVAL && gc_budget(&gc, 9) == 30000,
          "oversized timer interval rejected without changing existing budget");
}


static uint64_t fake_now;
static uint64_t fake_clock(void) { return fake_now; }
typedef struct {
    uint64_t cost;
    long result;
    uint32_t output, runs, mutes, kills;
    int cleared_before_kill;
} fake_node_t;
static long fake_run(void *ctx)
{
    fake_node_t *n = ctx;
    n->runs++;
    n->output = 0x3f800000;
    fake_now += n->cost;
    return n->result;
}
static void fake_mute(void *ctx)
{
    fake_node_t *n = ctx;
    n->output = 0;
    n->mutes++;
}
static void fake_kill(void *ctx)
{
    fake_node_t *n = ctx;
    n->kills++;
    n->cleared_before_kill = (n->output == 0);
}
static const budget_ops_t fake_ops = { fake_clock, fake_run, fake_mute, fake_kill };

static void test_executor(void)
{
    printf("- executor: partial writes, late return, death and forgiveness\n");
    budget_t b;
    uint64_t dt = 999;
    fake_node_t n = { .cost = 20, .result = 0 };
    budget_init(&b, 100, 3);
    CHECK(budget_execute(&b, &fake_ops, &n, &dt) == BUDGET_OK &&
          dt == 20 && n.output && !n.mutes, "clean output is preserved");
    n.cost = 110; n.result = BUDGET_PREEMPTED;
    CHECK(budget_execute(&b, &fake_ops, &n, &dt) == BUDGET_MUTE &&
          dt == 110 && !n.output && !n.kills, "preempted partial output is erased");
    n.result = 0;
    CHECK(budget_execute(&b, &fake_ops, &n, &dt) == BUDGET_MUTE &&
          !n.output && b.streak == 2, "late return cannot evade pending timer IRQ");
    n.cost = 20;
    CHECK(budget_execute(&b, &fake_ops, &n, &dt) == BUDGET_OK &&
          n.output && b.streak == 0, "transient offender recovers with fresh output");
    n.cost = 100;
    CHECK(budget_execute(&b, &fake_ops, &n, &dt) == BUDGET_MUTE,
          "exact boundary is over budget");
    budget_execute(&b, &fake_ops, &n, &dt);
    CHECK(budget_execute(&b, &fake_ops, &n, &dt) == BUDGET_KILL &&
          n.kills == 1 && n.cleared_before_kill, "third strike really kills after muting");
    uint32_t runs = n.runs;
    n.output = 1;
    CHECK(budget_execute(&b, &fake_ops, &n, &dt) == BUDGET_KILL &&
          n.runs == runs && n.kills == 1 && !n.output && dt == 0,
          "dead plugin never runs or kills twice and remains silent");
    budget_init(&b, 100, 3);
    n.result = -1; n.cost = 1;
    CHECK(budget_execute(&b, &fake_ops, &n, &dt) == BUDGET_FAULT &&
          b.killed && b.offences == 0 && !n.output, "fault is not forgiven as a clean block");
    budget_init(&b, 0, 3);
    CHECK(b.cycles == 1, "zero budget clamps to a finite tick");
    b.offences = UINT64_MAX;
    budget_account(&b, 1);
    CHECK(b.offences == UINT64_MAX, "offence total saturates instead of wrapping");
    budget_init(&b, UINT64_MAX, 3);
    uint32_t prior_runs = n.runs;
    CHECK(budget_execute(&b, &fake_ops, &n, &dt) == BUDGET_FAULT &&
          n.runs == prior_runs && !n.output && b.killed,
          "out-of-range timer cannot invoke an unbounded plugin");
    CHECK(budget_execute(&b, 0, &n, &dt) == BUDGET_FAULT && dt == 0,
          "invalid executor callbacks fail closed without invocation");
}

int main(void)
{
    printf("=== budget policy host tests (issue #78) ===\n");
    test_fair_share();
    test_escalation();
    test_forgiveness();
    test_kill_after_one();
    test_registry();
    test_executor();

    if (g_fail) {
        printf("BUDGET TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("BUDGET TESTS: ALL PASS\n");
    return 0;
}
