/* arch/arm64/plugin_time.h - per-plugin service-time reporting (Issue #77, M12)
 *
 * The sandbox contains memory (MMU) and syscalls (SVC gate), but not time: a
 * plugin that overspends its block budget starves the graph, and the audio
 * watchdog only sees the global overrun.  Enforcement (issue #78) needs
 * attribution first - this module reports WHERE each block's time went, per
 * plugin, without perturbing the audio path.
 *
 * The measurement itself lives in the graph executor: an audio worker with a
 * clock installed (audio_worker.c, issue #74) reads CNTPCT_EL0 around every
 * node's process callback and accumulates min / max / sum service time on the
 * node.  The accumulation window is the node's lifetime on its worker - reset
 * whenever the node is (re)assigned, which is exactly when its cost regime
 * changes (fresh plugin, different core).  A windowed ring per node (as
 * latency.c keeps for the one callback cadence) would cost 16 rings per
 * worker and a per-block copy; lifetime min/max/mean is what issue #78's
 * budget policy needs.
 *
 * Publication follows the latency-stats pattern (issue #22): the worker
 * publishes a seqlocked snapshot of its node table after each completed
 * block - hang pt_publish on the worker's publish hook - and a reporter on a
 * non-audio core renders it over UART:
 *
 *   plugin_time: pid=3 (heavy) runs=2400 min=330us max=352us mean=334us overruns=2
 *
 * All fixed-point integer math (the kernel builds -mgeneral-regs-only); the
 * cycles->us conversion is latency.c's.  Everything is pure C, unit-tested on
 * the host (make test-arm-ptime).
 */

#ifndef ARM64_PLUGIN_TIME_H
#define ARM64_PLUGIN_TIME_H

#include "audio_worker.h"
#include <stdint.h>

/* One node's published accounting (cycles, except where noted). */
typedef struct {
    uint32_t tag;                /* attribution id (plugin pid)             */
    uint64_t runs;               /* blocks executed                         */
    uint64_t overruns;           /* blocks missed (charged by aw_kick)      */
    uint64_t min, max, sum;      /* service time; min/max 0 until first run */
} pt_entry_t;

/* A worker's seqlocked stats board: written by that worker after every
 * completed block, read by the reporter with retry.  One writer, any
 * readers, nobody blocks. */
typedef struct pt_board {
    volatile uint32_t seq;       /* odd = publish in progress               */
    uint32_t          n;
    pt_entry_t        e[AW_MAX_NODES];
} pt_board_t;

void pt_board_init(pt_board_t *b);

/* Worker side: publish the worker's node table.  Signature-compatible with
 * the audio_worker publish hook:
 *   w->publish = pt_publish;  w->pub_ctx = &board;                        */
void pt_publish(audio_worker_t *w, void *board);

/* Reporter side: copy a consistent snapshot into out[cap]; returns the entry
 * count, or -1 if the board would not settle within `retries` attempts (the
 * writer publishes in microseconds, so a handful suffices). */
int pt_snapshot(const pt_board_t *b, pt_entry_t *out, int cap, int retries);

/* Render one entry as the stats line, e.g.
 *   plugin_time: pid=7 (sine) runs=999 min=3us max=12us mean=5us overruns=0
 * `name` may be NULL (the "(name) " part is omitted).  `cntfrq` converts
 * cycles to microseconds.  Returns characters written (NUL-terminated). */
int pt_render(const pt_entry_t *e, const char *name, uint64_t cntfrq,
              char *out, int cap);

#endif /* ARM64_PLUGIN_TIME_H */
