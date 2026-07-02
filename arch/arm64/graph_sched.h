/* arch/arm64/graph_sched.h - topology-aware graph partitioning (Issue #75, M11)
 *
 * Issue #74 gave each secondary core an audio worker that runs assigned nodes
 * once per block and never makes CPU0 wait.  This module decides WHICH nodes
 * run WHERE: it turns the audio graph (issue #27) into a per-worker execution
 * plan that honours the ring-buffer edges, and swaps a new plan in at a block
 * boundary without glitching the running audio.
 *
 * Execution model (documented in docs/graph-scheduling.md):
 *
 *   - Within one worker, nodes run in topological order, so a same-core edge
 *     has same-block semantics: the consumer sees the producer's output from
 *     the current block.
 *   - An edge whose endpoints land on different cores is PIPELINED: the
 *     consumer reads what the producer wrote last block (+1 block of latency
 *     per cross-core hop).  Workers therefore never wait on each other inside
 *     a block - the issue #74 contract survives partitioning.  When a plan
 *     makes an edge newly cross-core, its ring is reset and primed with one
 *     block of silence to put the pipeline in steady state.
 *   - Edges into the DAC are inherently pipelined: CPU0 drains its ring right
 *     after kicking the workers, so it reads the previous block's output.
 *
 * Placement is balance-first and deterministic: walking the topological
 * order, a node prefers the core of its first upstream producer (keeping
 * chains together, no added latency) until that core reaches its share
 * ceil(n_plugins / n_workers), else it takes the least-loaded core.
 * Cost-aware packing needs per-plugin service times and lands with issue #77.
 *
 * Plans move from the control plane to the audio core through a seqlocked
 * staging slot, in the spirit of graph_control's edge snapshot: the (single)
 * control-plane writer stages after every graph mutation - freely overwriting
 * an unconsumed older stage, so only the newest plan ever applies - and CPU0,
 * at a block boundary with every worker drained, copies the slot, verifies
 * the stage generation did not move, and only then commits.  A torn copy or
 * a busy worker just defers the swap to the next block.  Applying rewrites
 * the worker node tables (bounded: GRAPH_MAX_NODES) and resets/primes only
 * the edges whose placement actually changed - untouched edges keep
 * streaming.  Neither side ever blocks or spins; a worker stalled forever
 * (until issue #78 kills it) delays reconfiguration, never audio.
 *
 * Everything here is pure C on top of audio_graph/audio_worker and is
 * unit-tested on the host (make test-arm-gsched).
 */

#ifndef ARM64_GRAPH_SCHED_H
#define ARM64_GRAPH_SCHED_H

#include "audio_graph.h"
#include "audio_worker.h"
#include <stdint.h>

#define GS_MAX_WORKERS 3          /* CPU1-3; CPU0 is the audio core        */

#define GS_CORE_DAC   (-1)        /* the DAC "runs" on the audio core      */
#define GS_CORE_NONE  (-2)        /* slot not a live node in this plan     */

#define GS_OK          0
#define GS_ECYCLE    (-1)         /* graph has a cycle (toposort failed)   */
#define GS_EINVAL    (-3)         /* bad argument                          */

/* How a graph node executes on a worker (the kernel points these at the
 * plugin process-block trampoline; harnesses use plain callbacks). */
typedef struct {
    void (*run)(void *ctx);
    void  *ctx;
} gs_node_fn_t;

/* Edge placement captured at plan time (for priming decisions on apply). */
typedef struct {
    int   used;
    int   src, dst;               /* graph node indices                    */
    void *ring;                   /* ring backing the edge at plan time    */
    int   cross;                  /* endpoints on different cores          */
} gs_edge_t;

/* A computed assignment: node -> core, per-worker execution order, edges. */
typedef struct {
    int       valid;
    int       core[GRAPH_MAX_NODES];   /* worker slot, GS_CORE_DAC, GS_CORE_NONE */
    uint32_t  pid[GRAPH_MAX_NODES];    /* node pids captured for the stats line  */
    int       wnodes[GS_MAX_WORKERS][GRAPH_MAX_NODES]; /* topo order per worker  */
    int       n_wnodes[GS_MAX_WORKERS];
    gs_edge_t edge[GRAPH_MAX_EDGES];
    int       n_plugins;
    int       n_workers;               /* scheduling width this plan used        */
    int       cross_edges;             /* plugin->plugin edges that cross cores  */
} graph_plan_t;

/* Compute a plan for `g` over `n_workers` cores (clamped to 1..GS_MAX_WORKERS).
 * Deterministic.  Returns GS_OK or GS_ECYCLE. */
int graph_plan_compute(const audio_graph_t *g, int n_workers, graph_plan_t *p);

/* Reset an edge's ring at a placement change; `prime_silence` != 0 asks for
 * one block of silence so a newly cross-core edge starts in pipeline steady
 * state.  Supplied by the kernel/harness (it knows the ring type). */
typedef void (*gs_edge_reset_fn)(void *ring, int prime_silence, void *ctx);

/* The stage/apply handoff between the control plane and the audio core. */
typedef struct {
    graph_plan_t      staged;     /* seqlocked slot, control-plane writer  */
    volatile uint32_t stage_gen;  /* odd = stage in progress, even = stable */
    graph_plan_t      incoming;   /* CPU0's scratch copy of the slot       */
    uint32_t          applied_gen;/* stage_gen CPU0 last consumed          */
    graph_plan_t      active;     /* owned by CPU0                         */
    uint32_t          generation; /* bumped on every applied plan          */
    int               n_avail;    /* workers physically available          */
    int               n_workers;  /* scheduling width for the next stage   */
} graph_sched_t;

/* `n_avail` workers exist (CPU1..CPU1+n_avail-1); scheduling width starts at
 * n_avail and can be narrowed with graph_sched_set_workers. */
void graph_sched_init(graph_sched_t *s, int n_avail);

/* Set the scheduling width for subsequent stages (1..n_avail). */
int graph_sched_set_workers(graph_sched_t *s, int n_workers);

/* Control plane (single writer): compute a plan from `g` and hand it to the
 * audio core, overwriting any not-yet-applied older stage.  Returns GS_OK or
 * GS_ECYCLE (on a cycle the previous plan stays active). */
int graph_sched_stage(graph_sched_t *s, const audio_graph_t *g);

/* Audio core, at block start BEFORE kicking: if a plan is staged and every
 * worker in `workers[0..n_avail)` has drained, rewrite the worker node tables
 * from the plan (execution callbacks come from `fns`, indexed by graph node),
 * reset/prime the edges whose placement changed via `edge_reset`, bump the
 * generation, and release the mailbox.  Returns 1 if a plan was applied, 0
 * otherwise.  Never blocks: if a worker is still busy, it returns 0 and the
 * caller retries next block. */
int graph_sched_apply(graph_sched_t *s, audio_worker_t *workers,
                      const gs_node_fn_t *fns,
                      gs_edge_reset_fn edge_reset, void *reset_ctx);

/* Render the active assignment for the stats output, e.g.
 *   graph_sched: gen=3 workers=2 cross=1 n0(pid=7)->w0 n1(pid=9)->w1 dac->cpu0
 * Returns the number of characters written (NUL-terminated, truncates). */
int graph_sched_format(const graph_sched_t *s, char *out, int cap);

#endif /* ARM64_GRAPH_SCHED_H */
