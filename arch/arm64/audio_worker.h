/* arch/arm64/audio_worker.h - pinned per-core audio workers (Issue #74, M11)
 *
 * The audio core owns CPU0; M11 distributes graph work across CPU1-3.  Each
 * secondary core runs one audio worker: a loop that parks in WFE until the
 * audio core publishes a new block, executes its assigned nodes, publishes
 * completion, and parks again.
 *
 * The cadence contract, in the spirit of the SPSC ring (issue #25): each side
 * owns exactly one release-ordered sequence counter and never blocks on the
 * other.
 *
 *   - CPU0 kicks a worker at block start by publishing `block_seq` (aw_kick).
 *     If the worker has not finished everything it was previously asked to do,
 *     the kick is SKIPPED: the block is counted as an overrun against the
 *     worker and each of its nodes, and CPU0 moves on immediately.  A late or
 *     stalled worker can never make the audio core wait.
 *   - The worker executes the published block and answers by publishing
 *     `done_seq` (aw_worker_step).  Once it catches up, the next kick succeeds
 *     again - recovery is automatic.
 *
 * There are no locks and no syscalls anywhere on this path, only two
 * acquire/release counters per worker.  Every kick therefore ends in exactly
 * one of two ways: the block is eventually executed (`blocks`), or it is
 * skipped and accounted (`overruns`); blocks + overruns == kicks once the
 * worker has drained.
 *
 * Node assignment is the plugin-lifecycle integration point: load appends a
 * node (aw_assign), unload clears (aw_clear).  A worker with no nodes is never
 * kicked and parks in WFE.  Assignment is append/clear only and takes effect
 * at the next kick; topology-aware placement and rebalancing across workers is
 * issue #75.
 *
 * The protocol is pure C (GCC atomic builtins), unit-tested on the host with a
 * real two-thread kicker/worker pair (make test-arm-worker); only the
 * park/wake instructions (WFE/SEV) are AArch64-specific and compile to a CPU
 * relax on the host.
 */

#ifndef ARM64_AUDIO_WORKER_H
#define ARM64_AUDIO_WORKER_H

#include <stdint.h>

#define AW_MAX_NODES 16          /* mirrors GRAPH_MAX_NODES (audio_graph.h) */

/* One unit of per-block work: a plugin node's process callback.
 *
 * When the worker has a clock (issue #77), each run is timed and accumulated
 * here: min / max / sum of the node's service time in counter cycles, since
 * the node was assigned.  Assignment is also the moment a node's cost regime
 * changes (fresh plugin, new core), so the accumulation window is "this
 * node's lifetime on this worker" - reset by aw_assign, rendered as
 * min/max/mean in the latency-stats style by plugin_time.c. */
typedef struct {
    void   (*run)(void *ctx);
    void    *ctx;
    uint32_t tag;                /* attribution id (plugin pid); 0 = none   */
    uint64_t runs;               /* blocks this node actually executed      */
    uint64_t overruns;           /* blocks missed because its worker was late */
    uint64_t offences;           /* budget offences (issue #78; bumped by the
                                  * host's enforcement wrapper)             */
    uint64_t svc_min;            /* fastest run (cycles; ~0 until first run) */
    uint64_t svc_max;            /* slowest run (cycles)                    */
    uint64_t svc_sum;            /* total run time (cycles)                 */
} aw_node_t;

/* One worker, pinned to one secondary core.  The two sequence counters are
 * kept on separate cache lines: CPU0 writes `block_seq`, the worker writes
 * `done_seq`, and neither should steal the other's line per block. */
typedef struct audio_worker_s {
    /* ---- audio-core side (written by CPU0) ---- */
    volatile uint64_t block_seq __attribute__((aligned(64)));
    uint64_t          kicks;     /* kick attempts (published or skipped)     */
    uint64_t          overruns;  /* kicks skipped because the worker was late */

    /* ---- worker side (written by the worker core) ---- */
    volatile uint64_t done_seq __attribute__((aligned(64)));
    uint64_t          blocks;    /* blocks executed                          */
    volatile uint32_t online;    /* set by the worker once its loop runs     */
    volatile uint32_t stop;      /* ask the loop to exit                     */

    /* ---- assignment (control plane; applies at the next kick) ---- */
    volatile uint32_t n_nodes __attribute__((aligned(64)));
    aw_node_t         nodes[AW_MAX_NODES];
    uint32_t          cpu_id;    /* core this worker is pinned to            */

    /* ---- per-node time accounting (issue #77; both optional) ---- */
    uint64_t        (*clock)(void);  /* CNTPCT reader; NULL = accounting off */
    void            (*publish)(struct audio_worker_s *w, void *ctx);
    void             *pub_ctx;   /* fired after each completed block, on the
                                  * worker core - plugin_time.c's seqlock
                                  * board publisher hangs here               */
} audio_worker_t;

/* Reset a worker to empty/idle, pinned to `cpu_id`. */
void aw_init(audio_worker_t *w, uint32_t cpu_id);

/* Append a node (plugin load).  Returns the node slot, or -1 if full.  The
 * node runs from the next kick onward. */
int aw_assign(audio_worker_t *w, void (*run)(void *ctx), void *ctx);

/* Remove all nodes (plugin unload).  From the next kick the worker is treated
 * as empty and parks. */
void aw_clear(audio_worker_t *w);

/* Audio-core side, once per block on CPU0 with a monotonically increasing
 * `seq` (first block = 1).  Publishes the block to the worker and returns 1;
 * or, if the worker has not caught up with its previous kick, skips the block,
 * accounts one overrun to the worker and each assigned node, and returns 0.
 * An empty worker is not kicked (returns 1).  Never blocks, never spins. */
int aw_kick(audio_worker_t *w, uint64_t seq);

/* Worker side: execute one published block if there is one.  Returns 1 if a
 * block was run (completion published), 0 if there was nothing new and the
 * caller may park.  Pure - host tests drive it directly. */
int aw_worker_step(audio_worker_t *w);

/* The worker core's loop: mark online, then step/park (WFE) until `stop` is
 * set.  Hand this to smp_start_core with the worker as the argument. */
void aw_worker_loop(audio_worker_t *w);

/* Ask the loop to exit and wake the core so it notices. */
void aw_stop(audio_worker_t *w);

/* True once the worker has drained every published kick (done == block).
 * After stopping the kicker and waiting for this, blocks + overruns == kicks
 * holds exactly. */
int aw_drained(const audio_worker_t *w);

#endif /* ARM64_AUDIO_WORKER_H */
