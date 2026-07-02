/* arch/arm64/graph_sched.c - topology-aware graph partitioning (Issue #75, M11) */

#include "graph_sched.h"

/* Field-by-field plan copy.  Deliberately not a struct assignment: the kernel
 * builds -fno-builtin/-ffreestanding and a struct copy may lower to a memcpy
 * call that standalone harnesses do not link. */
static void plan_copy(graph_plan_t *dst, const graph_plan_t *src)
{
    dst->valid = src->valid;
    for (int i = 0; i < GRAPH_MAX_NODES; i++) {
        dst->core[i] = src->core[i];
        dst->pid[i]  = src->pid[i];
    }
    for (int k = 0; k < GS_MAX_WORKERS; k++) {
        dst->n_wnodes[k] = src->n_wnodes[k];
        for (int i = 0; i < GRAPH_MAX_NODES; i++)
            dst->wnodes[k][i] = src->wnodes[k][i];
    }
    for (int e = 0; e < GRAPH_MAX_EDGES; e++) {
        dst->edge[e].used  = src->edge[e].used;
        dst->edge[e].src   = src->edge[e].src;
        dst->edge[e].dst   = src->edge[e].dst;
        dst->edge[e].ring  = src->edge[e].ring;
        dst->edge[e].cross = src->edge[e].cross;
    }
    dst->n_plugins   = src->n_plugins;
    dst->n_workers   = src->n_workers;
    dst->cross_edges = src->cross_edges;
}

int graph_plan_compute(const audio_graph_t *g, int n_workers, graph_plan_t *p)
{
    if (n_workers < 1)
        n_workers = 1;
    if (n_workers > GS_MAX_WORKERS)
        n_workers = GS_MAX_WORKERS;

    p->valid = 0;
    for (int i = 0; i < GRAPH_MAX_NODES; i++) {
        p->core[i] = GS_CORE_NONE;
        p->pid[i]  = 0;
    }
    for (int k = 0; k < GS_MAX_WORKERS; k++)
        p->n_wnodes[k] = 0;
    for (int e = 0; e < GRAPH_MAX_EDGES; e++)
        p->edge[e].used = 0;
    p->n_plugins   = 0;
    p->n_workers   = n_workers;
    p->cross_edges = 0;

    int order[GRAPH_MAX_NODES];
    int count = audio_graph_toposort(g, order, GRAPH_MAX_NODES);
    if (count < 0)
        return GS_ECYCLE;

    int n_plugins = count - (g->dac_node >= 0 ? 1 : 0);
    p->n_plugins = n_plugins;

    /* Balance-first share: no core takes more than its ceil'd fair share, so
     * wide graphs spread out; within the share a node follows its producer. */
    int cap = (n_plugins + n_workers - 1) / n_workers;
    if (cap < 1)
        cap = 1;

    int load[GS_MAX_WORKERS] = {0, 0, 0};

    for (int i = 0; i < count; i++) {
        int n = order[i];
        p->pid[n] = g->nodes[n].pid;

        if (n == g->dac_node) {
            p->core[n] = GS_CORE_DAC;
            continue;
        }

        /* Prefer the core of the first upstream plugin producer (edge-table
         * order, deterministic): a chain stays on one core - and keeps
         * same-block latency - until that core is at its share. */
        int pick = -1;
        for (int e = 0; e < GRAPH_MAX_EDGES; e++) {
            if (!g->edges[e].used || g->edges[e].dst != n)
                continue;
            int up = p->core[g->edges[e].src];
            if (up >= 0) {
                if (load[up] < cap)
                    pick = up;
                break;
            }
        }
        if (pick < 0) {
            pick = 0;
            for (int k = 1; k < n_workers; k++)
                if (load[k] < load[pick])
                    pick = k;
        }

        p->core[n] = pick;
        p->wnodes[pick][p->n_wnodes[pick]++] = n;   /* topo order preserved */
        load[pick]++;
    }

    for (int e = 0; e < GRAPH_MAX_EDGES; e++) {
        if (!g->edges[e].used)
            continue;
        p->edge[e].used = 1;
        p->edge[e].src  = g->edges[e].src;
        p->edge[e].dst  = g->edges[e].dst;
        p->edge[e].ring = g->edges[e].ring;
        p->edge[e].cross = (p->core[g->edges[e].src] != p->core[g->edges[e].dst]);
        if (p->edge[e].cross && g->edges[e].dst != g->dac_node)
            p->cross_edges++;
    }

    p->valid = 1;
    return GS_OK;
}

void graph_sched_init(graph_sched_t *s, int n_avail)
{
    if (n_avail < 1)
        n_avail = 1;
    if (n_avail > GS_MAX_WORKERS)
        n_avail = GS_MAX_WORKERS;
    s->staged.valid   = 0;
    s->incoming.valid = 0;
    s->active.valid   = 0;
    s->stage_gen      = 0;
    s->applied_gen    = 0;
    s->generation     = 0;
    s->n_avail        = n_avail;
    s->n_workers      = n_avail;
}

int graph_sched_set_workers(graph_sched_t *s, int n_workers)
{
    if (n_workers < 1 || n_workers > s->n_avail)
        return GS_EINVAL;
    s->n_workers = n_workers;
    return GS_OK;
}

int graph_sched_stage(graph_sched_t *s, const audio_graph_t *g)
{
    /* Seqlock write: odd while the slot is being rewritten.  A newer stage
     * freely overwrites an unconsumed older one - only the newest plan ever
     * reaches the audio core. */
    uint32_t gen = s->stage_gen + 1u;                    /* odd */
    __atomic_store_n(&s->stage_gen, gen, __ATOMIC_RELEASE);

    int r = graph_plan_compute(g, s->n_workers, &s->staged);
    if (r != GS_OK)
        s->staged.valid = 0;         /* poison: CPU0 will skip this stage */

    __atomic_store_n(&s->stage_gen, gen + 1u, __ATOMIC_RELEASE);  /* even */
    return r;
}

/* Did edge `e` of the new plan change placement relative to the active plan?
 * New edges, rings swapped by a rewire, and same<->cross transitions all
 * count; an edge whose placement is unchanged keeps streaming untouched. */
static int edge_changed(const graph_plan_t *act, const gs_edge_t *ne, int e)
{
    if (!act->valid || !act->edge[e].used)
        return 1;
    const gs_edge_t *oe = &act->edge[e];
    if (oe->src != ne->src || oe->dst != ne->dst)
        return 1;
    if (oe->ring != ne->ring)
        return 1;
    return oe->cross != ne->cross;
}

int graph_sched_apply(graph_sched_t *s, audio_worker_t *workers,
                      const gs_node_fn_t *fns,
                      gs_edge_reset_fn edge_reset, void *reset_ctx)
{
    uint32_t g1 = __atomic_load_n(&s->stage_gen, __ATOMIC_ACQUIRE);
    if (g1 == s->applied_gen || (g1 & 1u))
        return 0;                    /* nothing new / stage mid-write */

    /* Reconfigure only between blocks: every worker must have answered every
     * kick.  A busy worker just defers the swap to the next block. */
    for (int k = 0; k < s->n_avail; k++)
        if (!aw_drained(&workers[k]))
            return 0;

    /* Seqlock read: copy the slot, then confirm the writer did not move.  A
     * torn copy retries next block; the writer is never blocked. */
    plan_copy(&s->incoming, &s->staged);
    if (__atomic_load_n(&s->stage_gen, __ATOMIC_ACQUIRE) != g1)
        return 0;

    if (!s->incoming.valid) {        /* poisoned stage (cycle): consume it */
        s->applied_gen = g1;
        return 0;
    }

    const graph_plan_t *np = &s->incoming;

    /* Put the pipeline of every re-placed edge into steady state before any
     * node can run under the new plan. */
    if (edge_reset)
        for (int e = 0; e < GRAPH_MAX_EDGES; e++) {
            if (!np->edge[e].used)
                continue;
            if (edge_changed(&s->active, &np->edge[e], e))
                edge_reset(np->edge[e].ring, np->edge[e].cross, reset_ctx);
        }

    /* Rewrite the worker node tables in per-worker topological order.  Each
     * slot is tagged with its plugin pid so the per-node time accounting
     * (issue #77) attributes to the right plugin. */
    for (int k = 0; k < s->n_avail; k++) {
        aw_clear(&workers[k]);
        if (k >= np->n_workers)
            continue;
        for (int i = 0; i < np->n_wnodes[k]; i++) {
            int n = np->wnodes[k][i];
            if (fns[n].run) {
                int slot = aw_assign(&workers[k], fns[n].run, fns[n].ctx);
                if (slot >= 0)
                    workers[k].nodes[slot].tag = np->pid[n];
            }
        }
    }

    plan_copy(&s->active, np);
    s->generation++;
    s->applied_gen = g1;
    return 1;
}

/* ---- stats rendering (freestanding, no printf) ------------------------ */

static int put_str(char *out, int cap, int at, const char *str)
{
    while (*str && at < cap - 1)
        out[at++] = *str++;
    return at;
}

static int put_u32(char *out, int cap, int at, uint32_t v)
{
    char tmp[10];
    int  n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v);
    while (n && at < cap - 1)
        out[at++] = tmp[--n];
    return at;
}

int graph_sched_format(const graph_sched_t *s, char *out, int cap)
{
    if (!out || cap < 1)
        return 0;

    int at = 0;
    at = put_str(out, cap, at, "graph_sched: gen=");
    at = put_u32(out, cap, at, s->generation);

    const graph_plan_t *p = &s->active;
    if (!p->valid) {
        at = put_str(out, cap, at, " (no plan)");
        out[at] = '\0';
        return at;
    }

    at = put_str(out, cap, at, " workers=");
    at = put_u32(out, cap, at, (uint32_t)p->n_workers);
    at = put_str(out, cap, at, " cross=");
    at = put_u32(out, cap, at, (uint32_t)p->cross_edges);

    for (int n = 0; n < GRAPH_MAX_NODES; n++) {
        if (p->core[n] == GS_CORE_NONE)
            continue;
        at = put_str(out, cap, at, " ");
        if (p->core[n] == GS_CORE_DAC) {
            at = put_str(out, cap, at, "dac->cpu0");
        } else {
            at = put_str(out, cap, at, "n");
            at = put_u32(out, cap, at, (uint32_t)n);
            at = put_str(out, cap, at, "(pid=");
            at = put_u32(out, cap, at, p->pid[n]);
            at = put_str(out, cap, at, ")->w");
            at = put_u32(out, cap, at, (uint32_t)p->core[n]);
        }
    }
    out[at] = '\0';
    return at;
}
