/* arch/arm64/graph_control.c - runtime audio-graph control plane (Issue #28) */

#include "graph_control.h"

void gc_init(graph_control_t *gc, const gc_ring_ops_t *ops)
{
    audio_graph_init(&gc->graph, (int (*)(uint32_t))0);
    gc->ops = *ops;
    gc->generation = 0;
    gc->on_change = (void (*)(void *))0;
    gc->on_change_ctx = (void *)0;
    for (int i = 0; i < GRAPH_MAX_NODES; i++) {
        gc->budgets[i].pid    = 0;
        gc->budgets[i].cycles = 0;
    }
}

int gc_set_budget(graph_control_t *gc, uint32_t pid, uint64_t cycles)
{
    if (pid == 0 || audio_graph_node_by_pid(&gc->graph, pid) < 0)
        return GC_ENODEV;

    int free_slot = -1;
    for (int i = 0; i < GRAPH_MAX_NODES; i++) {
        if (gc->budgets[i].pid == pid) {
            if (cycles)
                gc->budgets[i].cycles = cycles;
            else
                gc->budgets[i].pid = 0;        /* clear back to default */
            return GC_OK;
        }
        if (free_slot < 0 && gc->budgets[i].pid == 0)
            free_slot = i;
    }
    if (!cycles)
        return GC_OK;                          /* nothing set, nothing to clear */
    if (free_slot < 0)
        return GC_ENOMEM;
    gc->budgets[free_slot].pid    = pid;
    gc->budgets[free_slot].cycles = cycles;
    return GC_OK;
}

uint64_t gc_budget(const graph_control_t *gc, uint32_t pid)
{
    for (int i = 0; i < GRAPH_MAX_NODES; i++)
        if (gc->budgets[i].pid == pid)
            return gc->budgets[i].cycles;
    return 0;
}

void gc_set_on_change(graph_control_t *gc, void (*fn)(void *ctx), void *ctx)
{
    gc->on_change_ctx = ctx;
    gc->on_change = fn;
}

static void gc_changed(graph_control_t *gc)
{
    if (gc->on_change)
        gc->on_change(gc->on_change_ctx);
}

int gc_add_plugin(graph_control_t *gc, uint32_t pid)
{
    int n = audio_graph_add_node(&gc->graph, pid);
    if (n >= 0)
        gc_changed(gc);
    return n;
}

int gc_add_dac(graph_control_t *gc)
{
    int n = audio_graph_add_dac(&gc->graph);
    if (n >= 0)
        gc_changed(gc);
    return n;
}

/* Seqlock write fences: bump to odd before mutating, to even after, with
 * release ordering so a reader sees a coherent before/after edge list. */
static void gc_begin(graph_control_t *gc)
{
    __atomic_store_n(&gc->generation, gc->generation + 1u, __ATOMIC_RELEASE);
}

static void gc_end(graph_control_t *gc)
{
    __atomic_store_n(&gc->generation, gc->generation + 1u, __ATOMIC_RELEASE);
}

int gc_connect(graph_control_t *gc, uint32_t src_pid, uint32_t dst_pid)
{
    int si = audio_graph_node_by_pid(&gc->graph, src_pid);
    int di = audio_graph_node_by_pid(&gc->graph, dst_pid);
    if (si < 0 || di < 0)
        return GC_ENODEV;
    if (audio_graph_find_edge(&gc->graph, si, di) >= 0)
        return GC_EEXIST;

    void *ring = gc->ops.ring_new ? gc->ops.ring_new(gc->ops.ctx) : (void *)0;
    if (!ring)
        return GC_ENOMEM;

    gc_begin(gc);
    int e = audio_graph_connect(&gc->graph, si, di);
    if (e < 0) {
        gc_end(gc);
        if (gc->ops.ring_del) gc->ops.ring_del(gc->ops.ctx, ring);
        return GC_EINVAL;          /* lost a race / rule violation */
    }
    audio_graph_set_edge_ring(&gc->graph, e, ring);
    if (gc->ops.ring_map) {
        gc->ops.ring_map(gc->ops.ctx, src_pid, ring, /*input=*/0);
        gc->ops.ring_map(gc->ops.ctx, dst_pid, ring, /*input=*/1);
    }
    gc_end(gc);
    gc_changed(gc);
    return GC_OK;
}

int gc_disconnect(graph_control_t *gc, uint32_t src_pid, uint32_t dst_pid)
{
    int si = audio_graph_node_by_pid(&gc->graph, src_pid);
    int di = audio_graph_node_by_pid(&gc->graph, dst_pid);
    if (si < 0 || di < 0)
        return GC_ENODEV;
    if (audio_graph_find_edge(&gc->graph, si, di) < 0)
        return GC_ENOENT;

    gc_begin(gc);
    void *ring = audio_graph_disconnect(&gc->graph, si, di);
    if (gc->ops.ring_unmap && ring) {
        gc->ops.ring_unmap(gc->ops.ctx, src_pid, ring, 0);
        gc->ops.ring_unmap(gc->ops.ctx, dst_pid, ring, 1);
    }
    gc_end(gc);

    if (gc->ops.ring_del && ring)
        gc->ops.ring_del(gc->ops.ctx, ring);
    gc_changed(gc);
    return GC_OK;
}

static int collect_edges(const audio_graph_t *g, gc_edge_info_t *out, int max)
{
    int n = 0;
    for (int e = 0; e < GRAPH_MAX_EDGES && n < max; e++) {
        if (!g->edges[e].used)
            continue;
        out[n].src_pid = g->nodes[g->edges[e].src].pid;
        out[n].dst_pid = g->nodes[g->edges[e].dst].pid;
        n++;
    }
    return n;
}

int gc_list(graph_control_t *gc, gc_edge_info_t *out, int max)
{
    return collect_edges(&gc->graph, out, max);
}

uint32_t gc_generation(const graph_control_t *gc)
{
    return __atomic_load_n(&gc->generation, __ATOMIC_ACQUIRE);
}

int gc_snapshot(const graph_control_t *gc, gc_edge_info_t *out, int max, uint32_t *gen)
{
    for (;;) {
        uint32_t g1 = __atomic_load_n(&gc->generation, __ATOMIC_ACQUIRE);
        if (g1 & 1u)
            continue;                          /* a rewire is in progress */
        int n = collect_edges(&gc->graph, out, max);
        uint32_t g2 = __atomic_load_n(&gc->generation, __ATOMIC_ACQUIRE);
        if (g1 == g2) {                        /* stable across the read */
            if (gen) *gen = g1;
            return n;
        }
    }
}
