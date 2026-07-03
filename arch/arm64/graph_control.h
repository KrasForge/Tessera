/* arch/arm64/graph_control.h - runtime audio-graph control plane (Issue #28)
 *
 * Backs the host's control syscalls (sys_graph_connect / _disconnect / _list).
 * Connecting allocates a shared ring buffer, maps it into both processes, and
 * adds an edge; disconnecting unmaps and removes it.  The audio thread never
 * blocks on a rewire: every mutation bumps a seqlock generation, so a reader
 * can take a consistent atomic snapshot of the edge list and otherwise keep
 * running on the previous wiring.
 *
 * The ring lifecycle (allocate / free / map / unmap) is injected as callbacks,
 * so the control logic is independent of the MMU and is unit-tested on the host
 * (make test-arm-graph-control).
 */

#ifndef ARM64_GRAPH_CONTROL_H
#define ARM64_GRAPH_CONTROL_H

#include "audio_graph.h"
#include <stdint.h>

/* Error codes returned by the control operations (negative). */
#define GC_OK         0
#define GC_ENODEV   (-1)   /* a PID is not a registered node      */
#define GC_EEXIST   (-2)   /* the edge already exists             */
#define GC_ENOENT   (-3)   /* no such edge to disconnect          */
#define GC_ENOMEM   (-4)   /* ring allocation failed              */
#define GC_EINVAL   (-5)   /* bad argument                        */

typedef struct {
    uint32_t src_pid;
    uint32_t dst_pid;
} gc_edge_info_t;

/* Ring lifecycle hooks supplied by the kernel (NULL ctx is fine). */
typedef struct {
    void *(*ring_new)(void *ctx);                                  /* -> ring or NULL */
    void  (*ring_del)(void *ctx, void *ring);
    int   (*ring_map)(void *ctx, uint32_t pid, void *ring, int input); /* 0 ok */
    void  (*ring_unmap)(void *ctx, uint32_t pid, void *ring, int input);
    void  *ctx;
} gc_ring_ops_t;

typedef struct {
    audio_graph_t     graph;
    gc_ring_ops_t     ops;
    volatile uint32_t generation;   /* even = stable, odd = mid-rewire */
    void            (*on_change)(void *ctx);  /* fired after any successful  */
    void             *on_change_ctx;          /* mutation (issue #75)        */
    struct {                        /* per-plugin CPU budgets (issue #78)   */
        uint32_t pid;               /* 0 = free slot                        */
        uint64_t cycles;
    } budgets[GRAPH_MAX_NODES];
} graph_control_t;

void gc_init(graph_control_t *gc, const gc_ring_ops_t *ops);

/* Optional: `fn(ctx)` runs after every successful graph mutation (node add,
 * connect, disconnect) - the hook the graph scheduler uses to stage a new
 * node-to-core plan (issue #75). */
void gc_set_on_change(graph_control_t *gc, void (*fn)(void *ctx), void *ctx);

/* Register graph nodes by PID before wiring (DAC uses pid 0). */
int gc_add_plugin(graph_control_t *gc, uint32_t pid);
int gc_add_dac(graph_control_t *gc);

/* Register the single capture-input source node (issue #84).  Wire it as an
 * edge source (pid GRAPH_INPUT_PID) into any plugin or the DAC. */
int gc_add_input(graph_control_t *gc);

/* sys_graph_connect: wire src_pid -> dst_pid.  Allocates and maps a ring, adds
 * the edge.  Returns GC_OK or a negative GC_E* (GC_EEXIST for a duplicate). */
int gc_connect(graph_control_t *gc, uint32_t src_pid, uint32_t dst_pid);

/* sys_graph_disconnect: tear down src_pid -> dst_pid (unmap + free + remove). */
int gc_disconnect(graph_control_t *gc, uint32_t src_pid, uint32_t dst_pid);

/* sys_graph_list: copy the current edges into `out` (capacity `max`); returns
 * the number of edges. */
int gc_list(graph_control_t *gc, gc_edge_info_t *out, int max);

/* sys_plugin_set_budget: set the per-block CPU budget (counter cycles) for a
 * registered plugin (issue #78); cycles == 0 clears it back to the default
 * fair share.  Returns GC_OK, or GC_ENODEV for an unknown pid. */
int gc_set_budget(graph_control_t *gc, uint32_t pid, uint64_t cycles);

/* The budget set for `pid`, or 0 when none is set (the host then applies
 * budget_fair_share, see budget.h). */
uint64_t gc_budget(const graph_control_t *gc, uint32_t pid);

/* Current generation (acquire load).  Even between rewires. */
uint32_t gc_generation(const graph_control_t *gc);

/* Take a consistent atomic snapshot of the edge list for the audio thread:
 * retries while a rewire is in progress, never blocks the writer.  Returns the
 * edge count and stores the generation it observed in *gen. */
int gc_snapshot(const graph_control_t *gc, gc_edge_info_t *out, int max, uint32_t *gen);

#endif /* ARM64_GRAPH_CONTROL_H */
