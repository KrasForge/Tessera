/* arch/arm64/audio_graph.h - audio graph model (Issue #27, M6)
 *
 * M5 wired one plugin to the DAC; M6 chains several isolated plugins.  This is
 * the data model: a directed graph whose nodes are plugins and the DAC sink,
 * connected by shared-memory ring-buffer edges (issue #25).  Each block the
 * host walks the nodes in topological order - producers before consumers - so
 * every plugin sees its inputs already filled, with the DAC always processed
 * last.
 *
 * The graph structure, connection rules, and the topological sort are pure C
 * and unit-tested on the host (make test-arm-graph).  The ring buffer that
 * backs an edge is allocated and mapped by the kernel; the graph only holds an
 * opaque pointer to it.
 */

#ifndef ARM64_AUDIO_GRAPH_H
#define ARM64_AUDIO_GRAPH_H

#include <stdint.h>

#define GRAPH_MAX_NODES 16
#define GRAPH_MAX_EDGES 32

typedef enum {
    NODE_UNUSED = 0,
    NODE_PLUGIN,      /* an isolated plugin process            */
    NODE_DAC,         /* the output sink, always sorted last   */
} node_type_t;

typedef struct {
    node_type_t type;
    uint32_t    pid;       /* plugin PID (0 for the DAC sink)  */
} graph_node_t;

typedef struct {
    int   used;
    int   src;             /* producing node index            */
    int   dst;             /* consuming node index            */
    void *ring;            /* shared ring backing this edge (kernel-set) */
} graph_edge_t;

typedef struct {
    graph_node_t nodes[GRAPH_MAX_NODES];
    graph_edge_t edges[GRAPH_MAX_EDGES];
    int          n_nodes;
    int          n_edges;
    int          dac_node;                 /* index of the DAC, or -1 */
    int        (*pid_valid)(uint32_t pid); /* optional PID validator  */
} audio_graph_t;

/* Reset to an empty graph.  `validator` (may be NULL) is called by
 * audio_graph_add_node to reject invalid PIDs; if NULL, only pid != 0 is
 * required. */
void audio_graph_init(audio_graph_t *g, int (*validator)(uint32_t pid));

/* Add a plugin node for `pid`.  Returns the node index, or -1 if the PID is
 * invalid or the graph is full.  On error the graph is left unchanged. */
int audio_graph_add_node(audio_graph_t *g, uint32_t pid);

/* Add the single DAC sink node.  Returns its index, or -1 if one already
 * exists or the graph is full. */
int audio_graph_add_dac(audio_graph_t *g);

/* Connect producer `src` to consumer `dst` with an edge.  Returns the edge
 * index, or -1 on bad indices, a duplicate, or a full edge table.  The DAC
 * sink may be a dst but never a src. */
int audio_graph_connect(audio_graph_t *g, int src, int dst);

/* Attach the kernel-allocated ring buffer to an edge. */
void audio_graph_set_edge_ring(audio_graph_t *g, int edge, void *ring);

/* Node index for a plugin PID (or the DAC when pid == 0), or -1 if none. */
int audio_graph_node_by_pid(const audio_graph_t *g, uint32_t pid);

/* Edge index for the src->dst pair, or -1 if there is no such edge. */
int audio_graph_find_edge(const audio_graph_t *g, int src, int dst);

/* Remove the src->dst edge.  Returns the (now freed) edge's ring pointer so the
 * caller can release it, or NULL if there was no such edge. */
void *audio_graph_disconnect(audio_graph_t *g, int src, int dst);

/* Topologically sort the nodes into `order` (capacity `max`), producers before
 * consumers and the DAC last.  Returns the number of nodes written, or -1 if
 * the graph has a cycle or `max` is too small. */
int audio_graph_toposort(const audio_graph_t *g, int *order, int max);

#endif /* ARM64_AUDIO_GRAPH_H */
