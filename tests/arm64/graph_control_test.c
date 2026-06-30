/* tests/arm64/graph_control_test.c - host unit tests for the graph control
 * plane (Issue #28).
 *
 * Drives gc_connect / gc_disconnect / gc_list with mock ring callbacks and
 * checks the acceptance criteria: connect/disconnect at runtime, a duplicate
 * connect returns an error (not a second edge), gc_list reports accurate state
 * immediately after each operation, and the seqlock generation advances so the
 * audio thread can take a consistent atomic snapshot.
 *
 * Build/run via:  make test-arm-graph-control
 */

#include "graph_control.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Mock ring backend: hand out distinct non-NULL tokens and count operations. */
static int g_new, g_del, g_map, g_unmap;
static uintptr_t g_token = 0x1000;

static void *mock_new(void *ctx)   { (void)ctx; g_new++; return (void *)(g_token += 0x1000); }
static void  mock_del(void *ctx, void *r) { (void)ctx; (void)r; g_del++; }
static int   mock_map(void *ctx, uint32_t pid, void *r, int in)
                                   { (void)ctx; (void)pid; (void)r; (void)in; g_map++; return 0; }
static void  mock_unmap(void *ctx, uint32_t pid, void *r, int in)
                                   { (void)ctx; (void)pid; (void)r; (void)in; g_unmap++; }

#define SYNTH 11u
#define EFFECT 12u

int main(void)
{
    printf("=== Tessera graph-control tests (issue #28) ===\n");

    gc_ring_ops_t ops = { mock_new, mock_del, mock_map, mock_unmap, 0 };
    graph_control_t gc;
    gc_init(&gc, &ops);
    gc_add_plugin(&gc, SYNTH);
    gc_add_plugin(&gc, EFFECT);
    gc_add_dac(&gc);

    gc_edge_info_t edges[GRAPH_MAX_EDGES];
    CHECK(gc_list(&gc, edges, GRAPH_MAX_EDGES) == 0, "no edges initially");
    CHECK((gc_generation(&gc) & 1u) == 0, "generation starts even (stable)");

    /* Connect synth -> effect. */
    uint32_t g0 = gc_generation(&gc);
    CHECK(gc_connect(&gc, SYNTH, EFFECT) == GC_OK, "connect synth->effect");
    CHECK(g_new == 1 && g_map == 2, "ring allocated once, mapped into both ends");
    CHECK(gc_generation(&gc) == g0 + 2, "generation advanced by one rewire (even)");

    int n = gc_list(&gc, edges, GRAPH_MAX_EDGES);
    CHECK(n == 1 && edges[0].src_pid == SYNTH && edges[0].dst_pid == EFFECT,
          "list shows the new edge immediately");

    /* Duplicate connect must error and not create a second edge or ring. */
    CHECK(gc_connect(&gc, SYNTH, EFFECT) == GC_EEXIST, "duplicate connect rejected");
    CHECK(g_new == 1, "no ring allocated for the duplicate");
    CHECK(gc_list(&gc, edges, GRAPH_MAX_EDGES) == 1, "still exactly one edge");

    /* Connect effect -> DAC (pid 0). */
    CHECK(gc_connect(&gc, EFFECT, 0) == GC_OK, "connect effect->DAC");
    CHECK(gc_list(&gc, edges, GRAPH_MAX_EDGES) == 2, "two edges now");

    /* Unknown PID is rejected. */
    CHECK(gc_connect(&gc, 999u, EFFECT) == GC_ENODEV, "connect from unknown PID errors");
    CHECK(gc_disconnect(&gc, 999u, EFFECT) == GC_ENODEV, "disconnect unknown PID errors");

    /* Disconnect synth -> effect at runtime. */
    int del0 = g_del;
    CHECK(gc_disconnect(&gc, SYNTH, EFFECT) == GC_OK, "disconnect synth->effect");
    CHECK(g_unmap == 2 && g_del == del0 + 1, "ring unmapped from both ends and freed");
    n = gc_list(&gc, edges, GRAPH_MAX_EDGES);
    CHECK(n == 1 && edges[0].src_pid == EFFECT && edges[0].dst_pid == 0,
          "list reflects the disconnect immediately (only effect->DAC remains)");

    /* Disconnecting a missing edge errors. */
    CHECK(gc_disconnect(&gc, SYNTH, EFFECT) == GC_ENOENT, "re-disconnect errors");

    /* Atomic snapshot is consistent and reports the current generation. */
    uint32_t gen = 0;
    int sn = gc_snapshot(&gc, edges, GRAPH_MAX_EDGES, &gen);
    CHECK(sn == 1 && (gen & 1u) == 0, "snapshot is stable (even generation)");
    CHECK(gen == gc_generation(&gc), "snapshot generation matches current");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
