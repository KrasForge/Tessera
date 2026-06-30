/* arch/arm64/audio_graph.c - audio graph model (Issue #27, M6) */

#include "audio_graph.h"

void audio_graph_init(audio_graph_t *g, int (*validator)(uint32_t pid))
{
    for (int i = 0; i < GRAPH_MAX_NODES; i++) {
        g->nodes[i].type = NODE_UNUSED;
        g->nodes[i].pid  = 0;
    }
    for (int i = 0; i < GRAPH_MAX_EDGES; i++)
        g->edges[i].used = 0;
    g->n_nodes   = 0;
    g->n_edges   = 0;
    g->dac_node  = -1;
    g->pid_valid = validator;
}

static int alloc_node(audio_graph_t *g)
{
    for (int i = 0; i < GRAPH_MAX_NODES; i++)
        if (g->nodes[i].type == NODE_UNUSED)
            return i;
    return -1;
}

int audio_graph_add_node(audio_graph_t *g, uint32_t pid)
{
    /* Validate the PID before touching the graph, so a bad PID cannot corrupt
     * any existing state. */
    if (pid == 0)
        return -1;
    if (g->pid_valid && !g->pid_valid(pid))
        return -1;

    int i = alloc_node(g);
    if (i < 0)
        return -1;

    g->nodes[i].type = NODE_PLUGIN;
    g->nodes[i].pid  = pid;
    g->n_nodes++;
    return i;
}

int audio_graph_add_dac(audio_graph_t *g)
{
    if (g->dac_node >= 0)
        return -1;                 /* only one sink */

    int i = alloc_node(g);
    if (i < 0)
        return -1;

    g->nodes[i].type = NODE_DAC;
    g->nodes[i].pid  = 0;
    g->n_nodes++;
    g->dac_node = i;
    return i;
}

static int node_live(const audio_graph_t *g, int n)
{
    return n >= 0 && n < GRAPH_MAX_NODES && g->nodes[n].type != NODE_UNUSED;
}

int audio_graph_connect(audio_graph_t *g, int src, int dst)
{
    if (!node_live(g, src) || !node_live(g, dst) || src == dst)
        return -1;
    /* The DAC is a pure sink: it can be a destination but never a source. */
    if (g->nodes[src].type == NODE_DAC)
        return -1;

    for (int e = 0; e < GRAPH_MAX_EDGES; e++)
        if (g->edges[e].used && g->edges[e].src == src && g->edges[e].dst == dst)
            return -1;             /* duplicate edge */

    for (int e = 0; e < GRAPH_MAX_EDGES; e++) {
        if (!g->edges[e].used) {
            g->edges[e].used = 1;
            g->edges[e].src  = src;
            g->edges[e].dst  = dst;
            g->edges[e].ring = (void *)0;
            g->n_edges++;
            return e;
        }
    }
    return -1;                      /* edge table full */
}

void audio_graph_set_edge_ring(audio_graph_t *g, int edge, void *ring)
{
    if (edge >= 0 && edge < GRAPH_MAX_EDGES && g->edges[edge].used)
        g->edges[edge].ring = ring;
}

/* Kahn's algorithm.  Produces a topological order; afterward the DAC node is
 * forced to the very end (it is a sink, so nothing depends on it and moving it
 * last never violates the ordering). */
int audio_graph_toposort(const audio_graph_t *g, int *order, int max)
{
    int indeg[GRAPH_MAX_NODES];
    for (int i = 0; i < GRAPH_MAX_NODES; i++)
        indeg[i] = -1;                 /* -1 marks a non-node slot */
    for (int i = 0; i < GRAPH_MAX_NODES; i++)
        if (g->nodes[i].type != NODE_UNUSED)
            indeg[i] = 0;
    for (int e = 0; e < GRAPH_MAX_EDGES; e++)
        if (g->edges[e].used)
            indeg[g->edges[e].dst]++;

    if (max < g->n_nodes)
        return -1;

    int count = 0;
    /* Repeatedly emit a zero-in-degree node and relax its out-edges. */
    for (int iter = 0; iter < g->n_nodes; iter++) {
        int pick = -1;
        for (int i = 0; i < GRAPH_MAX_NODES; i++)
            if (indeg[i] == 0) { pick = i; break; }
        if (pick < 0)
            return -1;                 /* remaining nodes form a cycle */

        order[count++] = pick;
        indeg[pick] = -2;              /* emitted */
        for (int e = 0; e < GRAPH_MAX_EDGES; e++)
            if (g->edges[e].used && g->edges[e].src == pick)
                indeg[g->edges[e].dst]--;
    }

    /* Force the DAC sink to the end. */
    if (g->dac_node >= 0) {
        int pos = -1;
        for (int i = 0; i < count; i++)
            if (order[i] == g->dac_node) { pos = i; break; }
        if (pos >= 0)
            for (int i = pos; i < count - 1; i++)
                order[i] = order[i + 1];
        order[count - 1] = g->dac_node;
    }
    return count;
}
