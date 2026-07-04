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
    g->n_nodes    = 0;
    g->n_edges    = 0;
    g->dac_node   = -1;
    g->input_node = -1;
    g->pid_valid  = validator;
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

    g->nodes[i].type   = NODE_PLUGIN;
    g->nodes[i].pid    = pid;
    g->nodes[i].in_ch  = 2;
    g->nodes[i].out_ch = 2;
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

    g->nodes[i].type   = NODE_DAC;
    g->nodes[i].pid    = 0;
    g->nodes[i].in_ch  = 2;
    g->nodes[i].out_ch = 2;
    g->n_nodes++;
    g->dac_node = i;
    return i;
}

int audio_graph_add_input(audio_graph_t *g)
{
    if (g->input_node >= 0)
        return -1;                 /* only one capture source */

    int i = alloc_node(g);
    if (i < 0)
        return -1;

    g->nodes[i].type   = NODE_INPUT;
    g->nodes[i].pid    = GRAPH_INPUT_PID;
    g->nodes[i].in_ch  = 2;
    g->nodes[i].out_ch = 2;
    g->n_nodes++;
    g->input_node = i;
    return i;
}

static int node_live(const audio_graph_t *g, int n)
{
    return n >= 0 && n < GRAPH_MAX_NODES && g->nodes[n].type != NODE_UNUSED;
}

static int connect_ex(audio_graph_t *g, int src, int dst, int feedback)
{
    if (!node_live(g, src) || !node_live(g, dst) || src == dst)
        return -1;
    /* The DAC is a pure sink (never a source); the input is a pure source
     * (never a destination). */
    if (g->nodes[src].type == NODE_DAC)
        return -1;
    if (g->nodes[dst].type == NODE_INPUT)
        return -1;

    for (int e = 0; e < GRAPH_MAX_EDGES; e++)
        if (g->edges[e].used && g->edges[e].src == src && g->edges[e].dst == dst)
            return -1;             /* duplicate edge */

    /* Channel compatibility: the producer's output width must match the
     * consumer's input width.  Stereo defaults (2/2) always match, so existing
     * graphs are unaffected; a multi-channel plugin only wires to a matching
     * one. */
    if (g->nodes[src].out_ch != g->nodes[dst].in_ch)
        return -1;

    for (int e = 0; e < GRAPH_MAX_EDGES; e++) {
        if (!g->edges[e].used) {
            g->edges[e].used     = 1;
            g->edges[e].src      = src;
            g->edges[e].dst      = dst;
            g->edges[e].feedback = feedback;
            g->edges[e].channels = g->nodes[src].out_ch;
            g->edges[e].ring     = (void *)0;
            g->n_edges++;
            return e;
        }
    }
    return -1;                      /* edge table full */
}

void audio_graph_set_channels(audio_graph_t *g, int node, uint16_t in_ch, uint16_t out_ch)
{
    if (!node_live(g, node))
        return;
    /* A count of 0 leaves that side unchanged, so callers can set just one. */
    if (in_ch)
        g->nodes[node].in_ch = in_ch;
    if (out_ch)
        g->nodes[node].out_ch = out_ch;
}

uint16_t audio_graph_edge_channels(const audio_graph_t *g, int e)
{
    if (e < 0 || e >= GRAPH_MAX_EDGES || !g->edges[e].used)
        return 0;
    return g->edges[e].channels;
}

int audio_graph_connect(audio_graph_t *g, int src, int dst)
{
    return connect_ex(g, src, dst, 0);
}

int audio_graph_connect_feedback(audio_graph_t *g, int src, int dst)
{
    return connect_ex(g, src, dst, 1);
}

int audio_graph_edge_is_feedback(const audio_graph_t *g, int e)
{
    return (e >= 0 && e < GRAPH_MAX_EDGES && g->edges[e].used && g->edges[e].feedback);
}

void audio_graph_set_edge_ring(audio_graph_t *g, int edge, void *ring)
{
    if (edge >= 0 && edge < GRAPH_MAX_EDGES && g->edges[edge].used)
        g->edges[edge].ring = ring;
}

int audio_graph_node_by_pid(const audio_graph_t *g, uint32_t pid)
{
    for (int i = 0; i < GRAPH_MAX_NODES; i++) {
        if (g->nodes[i].type == NODE_UNUSED)
            continue;
        if (pid == 0 && g->nodes[i].type == NODE_DAC)
            return i;
        if (pid == GRAPH_INPUT_PID && g->nodes[i].type == NODE_INPUT)
            return i;
        if (pid != 0 && pid != GRAPH_INPUT_PID &&
            g->nodes[i].type == NODE_PLUGIN && g->nodes[i].pid == pid)
            return i;
    }
    return -1;
}

int audio_graph_find_edge(const audio_graph_t *g, int src, int dst)
{
    for (int e = 0; e < GRAPH_MAX_EDGES; e++)
        if (g->edges[e].used && g->edges[e].src == src && g->edges[e].dst == dst)
            return e;
    return -1;
}

void *audio_graph_disconnect(audio_graph_t *g, int src, int dst)
{
    int e = audio_graph_find_edge(g, src, dst);
    if (e < 0)
        return (void *)0;
    void *ring = g->edges[e].ring;
    g->edges[e].used = 0;
    g->edges[e].ring = (void *)0;
    g->n_edges--;
    return ring;
}

void audio_graph_remove_node(audio_graph_t *g, int n)
{
    if (n < 0 || n >= GRAPH_MAX_NODES || g->nodes[n].type == NODE_UNUSED)
        return;
    for (int e = 0; e < GRAPH_MAX_EDGES; e++)
        if (g->edges[e].used && (g->edges[e].src == n || g->edges[e].dst == n)) {
            g->edges[e].used = 0;
            g->edges[e].ring = (void *)0;
            g->n_edges--;
        }
    if (g->dac_node == n)
        g->dac_node = -1;
    if (g->input_node == n)
        g->input_node = -1;
    g->nodes[n].type = NODE_UNUSED;
    g->nodes[n].pid  = 0;
    g->n_nodes--;
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
    /* Feedback edges carry the previous block, so they impose no same-block
     * ordering constraint - exclude them from the in-degree, which is exactly
     * what breaks their cycle for scheduling. */
    for (int e = 0; e < GRAPH_MAX_EDGES; e++)
        if (g->edges[e].used && !g->edges[e].feedback)
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
            if (g->edges[e].used && !g->edges[e].feedback && g->edges[e].src == pick)
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
