/* tests/arm64/graph_sched_test.c - host unit tests for the topology-aware
 * graph partitioner (Issue #75).
 *
 * The planner, the seqlocked stage/apply handoff, and the edge reset/prime
 * rules are pure C, so everything that must be right for a glitch-free
 * repartition is checked on the host: deterministic balance-first placement
 * that keeps chains together up to the fair share, cycle rejection, DAC
 * placement on the audio core, apply deferred while a worker is busy or a
 * stage is torn, only-the-newest-stage-wins overwrite semantics, and
 * reset/prime fired exactly for the edges whose placement changed.
 *
 * Build/run via:  make test-arm-gsched
 */

#include "graph_sched.h"
#include "graph_control.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Fake rings are just distinct addresses. */
static int g_ringmem[8];

/* Recorded edge_reset invocations. */
#define MAX_RESETS 16
static struct { void *ring; int prime; } g_resets[MAX_RESETS];
static int g_n_resets;

static void reset_cb(void *ring, int prime, void *ctx)
{
    (void)ctx;
    if (g_n_resets < MAX_RESETS) {
        g_resets[g_n_resets].ring  = ring;
        g_resets[g_n_resets].prime = prime;
    }
    g_n_resets++;
}

static int reset_seen(void *ring, int prime)
{
    for (int i = 0; i < g_n_resets && i < MAX_RESETS; i++)
        if (g_resets[i].ring == ring && g_resets[i].prime == prime)
            return 1;
    return 0;
}

/* Node callbacks for the workers. */
static void bump(void *ctx) { (*(uint64_t *)ctx)++; }

/* ---- planner ---- */

static void test_plan_chain(void)
{
    printf("- planner: chains, capacity split, DAC placement\n");
    audio_graph_t g;
    audio_graph_init(&g, 0);
    int a = audio_graph_add_node(&g, 10);        /* synth  */
    int b = audio_graph_add_node(&g, 11);        /* filter */
    int d = audio_graph_add_dac(&g);
    int e_ab = audio_graph_connect(&g, a, b);
    int e_bd = audio_graph_connect(&g, b, d);
    audio_graph_set_edge_ring(&g, e_ab, &g_ringmem[0]);
    audio_graph_set_edge_ring(&g, e_bd, &g_ringmem[1]);

    graph_plan_t p;
    CHECK(graph_plan_compute(&g, 1, &p) == GS_OK, "1-worker plan computes");
    CHECK(p.core[a] == 0 && p.core[b] == 0, "chain stays on one core");
    CHECK(p.core[d] == GS_CORE_DAC, "DAC stays on the audio core");
    CHECK(p.cross_edges == 0, "no plugin cross edges on one core");
    CHECK(p.edge[e_ab].cross == 0, "same-core edge is same-block");
    CHECK(p.edge[e_bd].cross == 1, "edge into the DAC is pipelined");
    CHECK(p.n_wnodes[0] == 2 && p.wnodes[0][0] == a && p.wnodes[0][1] == b,
          "worker order is topological");

    CHECK(graph_plan_compute(&g, 2, &p) == GS_OK, "2-worker plan computes");
    CHECK(p.core[a] == 0 && p.core[b] == 1,
          "fair share splits the chain across cores");
    CHECK(p.cross_edges == 1 && p.edge[e_ab].cross == 1,
          "the split edge is pipelined and counted");

    graph_plan_t q;
    graph_plan_compute(&g, 2, &q);
    CHECK(memcmp(q.core, p.core, sizeof(q.core)) == 0, "planning is deterministic");
}

static void test_plan_shapes(void)
{
    printf("- planner: diamond balance, longer chain, cycle rejection\n");
    audio_graph_t g;
    audio_graph_init(&g, 0);
    int a = audio_graph_add_node(&g, 1);
    int b = audio_graph_add_node(&g, 2);
    int c = audio_graph_add_node(&g, 3);
    int d = audio_graph_add_node(&g, 4);
    audio_graph_connect(&g, a, b);
    audio_graph_connect(&g, a, c);
    audio_graph_connect(&g, b, d);
    audio_graph_connect(&g, c, d);

    graph_plan_t p;
    CHECK(graph_plan_compute(&g, 2, &p) == GS_OK, "diamond plan computes");
    CHECK(p.core[a] == 0 && p.core[b] == 0, "chain head follows its producer");
    CHECK(p.core[c] == 1 && p.core[d] == 1, "overflow balances to the other core");
    CHECK(p.n_plugins == 4, "plugin count");

    /* A 4-chain on 2 workers: two per core, one cross hop. */
    audio_graph_t h;
    audio_graph_init(&h, 0);
    int n0 = audio_graph_add_node(&h, 1);
    int n1 = audio_graph_add_node(&h, 2);
    int n2 = audio_graph_add_node(&h, 3);
    int n3 = audio_graph_add_node(&h, 4);
    audio_graph_connect(&h, n0, n1);
    audio_graph_connect(&h, n1, n2);
    audio_graph_connect(&h, n2, n3);
    CHECK(graph_plan_compute(&h, 2, &p) == GS_OK, "4-chain plan computes");
    CHECK(p.core[n0] == 0 && p.core[n1] == 0 && p.core[n2] == 1 && p.core[n3] == 1,
          "4-chain packs two per core");
    CHECK(p.cross_edges == 1, "exactly one cross hop");

    /* Cycle: A -> B -> A. */
    audio_graph_t cyc;
    audio_graph_init(&cyc, 0);
    int x = audio_graph_add_node(&cyc, 1);
    int y = audio_graph_add_node(&cyc, 2);
    audio_graph_connect(&cyc, x, y);
    audio_graph_connect(&cyc, y, x);
    CHECK(graph_plan_compute(&cyc, 2, &p) == GS_ECYCLE, "cycle is rejected");
}

/* ---- stage/apply ---- */

static void test_stage_apply(void)
{
    printf("- stage/apply: swap at a drained boundary, prime what changed\n");
    audio_graph_t g;
    audio_graph_init(&g, 0);
    int a = audio_graph_add_node(&g, 10);
    int b = audio_graph_add_node(&g, 11);
    int d = audio_graph_add_dac(&g);
    int e_ab = audio_graph_connect(&g, a, b);
    int e_bd = audio_graph_connect(&g, b, d);
    audio_graph_set_edge_ring(&g, e_ab, &g_ringmem[0]);
    audio_graph_set_edge_ring(&g, e_bd, &g_ringmem[1]);

    audio_worker_t w[GS_MAX_WORKERS];
    for (int k = 0; k < GS_MAX_WORKERS; k++)
        aw_init(&w[k], (uint32_t)(k + 1));

    uint64_t ra = 0, rb = 0;
    gs_node_fn_t fns[GRAPH_MAX_NODES] = {{0, 0}};
    fns[a].run = bump; fns[a].ctx = &ra;
    fns[b].run = bump; fns[b].ctx = &rb;

    graph_sched_t s;
    graph_sched_init(&s, 2);
    CHECK(graph_sched_set_workers(&s, 1) == GS_OK, "narrow to one worker");
    CHECK(graph_sched_stage(&s, &g) == GS_OK, "stage the 1-worker plan");

    g_n_resets = 0;
    CHECK(graph_sched_apply(&s, w, fns, reset_cb, 0) == 1, "initial apply");
    CHECK(s.generation == 1, "generation bumped");
    CHECK(w[0].n_nodes == 2 && w[1].n_nodes == 0, "both nodes on worker 0");
    CHECK(w[0].nodes[0].tag == 10 && w[0].nodes[1].tag == 11,
          "nodes tagged with their plugin pids (issue #77)");
    CHECK(reset_seen(&g_ringmem[1], 1), "DAC edge reset+primed (pipelined)");
    CHECK(reset_seen(&g_ringmem[0], 0), "same-core edge reset unprimed");
    CHECK(g_n_resets == 2, "no other edge touched");

    /* Nothing staged: apply is a no-op. */
    CHECK(graph_sched_apply(&s, w, fns, reset_cb, 0) == 0, "no re-apply without a stage");

    /* Split across two workers; a busy worker defers the swap. */
    CHECK(graph_sched_set_workers(&s, 2) == GS_OK, "widen to two workers");
    CHECK(graph_sched_stage(&s, &g) == GS_OK, "stage the split plan");
    aw_kick(&w[0], 1);                        /* worker 0 now has work due  */
    CHECK(graph_sched_apply(&s, w, fns, reset_cb, 0) == 0, "apply defers while busy");
    while (aw_worker_step(&w[0]))
        ;                                      /* worker answers the block  */
    g_n_resets = 0;
    CHECK(graph_sched_apply(&s, w, fns, reset_cb, 0) == 1, "apply lands once drained");
    CHECK(w[0].n_nodes == 1 && w[1].n_nodes == 1, "one node per worker");
    CHECK(s.active.core[a] != s.active.core[b], "chain is split");
    CHECK(reset_seen(&g_ringmem[0], 1), "same->cross edge reset+primed");
    CHECK(!reset_seen(&g_ringmem[1], 1) && !reset_seen(&g_ringmem[1], 0),
          "unchanged DAC edge left streaming");
    CHECK(g_n_resets == 1, "exactly one edge touched");

    /* Back to one worker: cross->same resets without priming. */
    graph_sched_set_workers(&s, 1);
    graph_sched_stage(&s, &g);
    g_n_resets = 0;
    CHECK(graph_sched_apply(&s, w, fns, reset_cb, 0) == 1, "merge applies");
    CHECK(reset_seen(&g_ringmem[0], 0), "cross->same edge reset unprimed");
    CHECK(g_n_resets == 1, "only the merged edge touched");

    /* A rewire that swaps the ring under the same src->dst re-primes. */
    audio_graph_set_edge_ring(&g, e_ab, &g_ringmem[2]);
    graph_sched_set_workers(&s, 2);
    graph_sched_stage(&s, &g);
    g_n_resets = 0;
    CHECK(graph_sched_apply(&s, w, fns, reset_cb, 0) == 1, "rewired plan applies");
    CHECK(reset_seen(&g_ringmem[2], 1), "new ring reset+primed");

    /* Stats line. */
    char line[160];
    int n = graph_sched_format(&s, line, sizeof line);
    CHECK(n > 0 && strstr(line, "graph_sched: gen=4"), "stats: generation");
    CHECK(strstr(line, "workers=2") && strstr(line, "cross=1"), "stats: shape");
    CHECK(strstr(line, "(pid=10)->w") && strstr(line, "(pid=11)->w"), "stats: nodes");
    CHECK(strstr(line, "dac->cpu0"), "stats: DAC");
}

static void test_overwrite_and_torn(void)
{
    printf("- staging: newest stage wins; a torn stage never applies\n");
    audio_graph_t g;
    audio_graph_init(&g, 0);
    int a = audio_graph_add_node(&g, 7);
    (void)a;

    audio_worker_t w[GS_MAX_WORKERS];
    for (int k = 0; k < GS_MAX_WORKERS; k++)
        aw_init(&w[k], (uint32_t)(k + 1));
    gs_node_fn_t fns[GRAPH_MAX_NODES] = {{0, 0}};
    uint64_t ra = 0;
    fns[a].run = bump; fns[a].ctx = &ra;

    graph_sched_t s;
    graph_sched_init(&s, 2);

    /* Two stages back to back: only the newest is ever applied. */
    graph_sched_stage(&s, &g);
    int b = audio_graph_add_node(&g, 8);
    uint64_t rb = 0;
    fns[b].run = bump; fns[b].ctx = &rb;
    graph_sched_stage(&s, &g);
    CHECK(graph_sched_apply(&s, w, fns, reset_cb, 0) == 1, "apply consumes the slot");
    CHECK(s.active.n_plugins == 2, "the newest stage won");
    CHECK(graph_sched_apply(&s, w, fns, reset_cb, 0) == 0, "older stage is gone");

    /* A stage frozen mid-write (odd gen) must not apply. */
    graph_sched_stage(&s, &g);
    __atomic_store_n(&s.stage_gen, s.stage_gen + 1u, __ATOMIC_RELEASE);
    CHECK(graph_sched_apply(&s, w, fns, reset_cb, 0) == 0, "mid-write stage deferred");
    __atomic_store_n(&s.stage_gen, s.stage_gen + 1u, __ATOMIC_RELEASE);

    /* A poisoned stage (cycle) is consumed without touching the plan. */
    audio_graph_connect(&g, a, b);
    audio_graph_connect(&g, b, a);
    uint32_t gen_before = s.generation;
    CHECK(graph_sched_stage(&s, &g) == GS_ECYCLE, "cycle reported at stage");
    CHECK(graph_sched_apply(&s, w, fns, reset_cb, 0) == 0, "poisoned stage skipped");
    CHECK(s.generation == gen_before && s.active.n_plugins == 2,
          "previous plan stays active");
}

/* ---- graph_control hook ---- */

static void *hook_ring_new(void *ctx) { (void)ctx; return &g_ringmem[3]; }

static graph_sched_t *g_hook_sched;
static graph_control_t *g_hook_gc;
static int g_hook_fired;

static void stage_hook(void *ctx)
{
    (void)ctx;
    g_hook_fired++;
    graph_sched_stage(g_hook_sched, &g_hook_gc->graph);
}

static void test_gc_hook(void)
{
    printf("- graph_control hook: every mutation stages a fresh plan\n");
    gc_ring_ops_t ops = { hook_ring_new, 0, 0, 0, 0 };
    graph_control_t gc;
    graph_sched_t s;
    gc_init(&gc, &ops);
    graph_sched_init(&s, 2);
    g_hook_sched = &s;
    g_hook_gc = &gc;
    g_hook_fired = 0;
    gc_set_on_change(&gc, stage_hook, 0);

    CHECK(gc_add_plugin(&gc, 5) >= 0 && g_hook_fired == 1, "load fires the hook");
    CHECK(gc_add_dac(&gc) >= 0 && g_hook_fired == 2, "DAC add fires the hook");
    CHECK(gc_connect(&gc, 5, 0) == GC_OK && g_hook_fired == 3, "wire fires the hook");
    CHECK(gc_connect(&gc, 5, 0) == GC_EEXIST && g_hook_fired == 3,
          "a failed mutation does not fire it");
    CHECK(gc_disconnect(&gc, 5, 0) == GC_OK && g_hook_fired == 4, "unwire fires the hook");

    /* The staged plan reflects the last mutation (edge gone again). */
    audio_worker_t w[GS_MAX_WORKERS];
    for (int k = 0; k < GS_MAX_WORKERS; k++)
        aw_init(&w[k], (uint32_t)(k + 1));
    gs_node_fn_t fns[GRAPH_MAX_NODES] = {{0, 0}};
    uint64_t r = 0;
    int n5 = audio_graph_node_by_pid(&gc.graph, 5);
    fns[n5].run = bump; fns[n5].ctx = &r;
    CHECK(graph_sched_apply(&s, w, fns, reset_cb, 0) == 1, "hooked stage applies");
    CHECK(s.active.n_plugins == 1 && s.active.edge[0].used == 0,
          "plan matches the post-unwire graph");
}

int main(void)
{
    printf("=== graph scheduler host tests (issue #75) ===\n");
    test_plan_chain();
    test_plan_shapes();
    test_stage_apply();
    test_overwrite_and_torn();
    test_gc_hook();

    if (g_fail) {
        printf("GSCHED TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("GSCHED TESTS: ALL PASS\n");
    return 0;
}
