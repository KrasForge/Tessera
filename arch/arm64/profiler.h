/* arch/arm64/profiler.h - per-plugin profiler (Theme G, issue #129)
 *
 * The M12 per-plugin time accounting (plugin_time.c, #77) already measures each
 * node's service time.  The profiler is the aggregation on top: it turns a
 * published snapshot into per-plugin CPU load (mean service time as a fraction
 * of the block period), peak time, overrun count, and the graph's remaining
 * headroom - the numbers a `prof` shell command and the on-device meter show.
 *
 * Pure fixed-point integer math (the kernel builds -mgeneral-regs-only); load
 * and headroom are carried in per-mille (0..1000 = 0..100%).  Unit-tested on
 * the host (make test-arm-profiler).
 */

#ifndef ARM64_PROFILER_H
#define ARM64_PROFILER_H

#include "plugin_time.h"
#include <stdint.h>

typedef struct {
    uint32_t pid;
    uint32_t runs;
    uint32_t mean_us;        /* mean service time, microseconds       */
    uint32_t max_us;         /* peak service time, microseconds       */
    uint32_t load_permille;  /* mean_us / block_us, in per-mille      */
    uint32_t overruns;
} prof_row_t;

/* Convert a cycle count to microseconds: us = cycles * 1e6 / cntfrq (rounded).
 * cntfrq is the timer frequency (CNTFRQ_EL0). */
uint32_t prof_cyc_to_us(uint64_t cycles, uint64_t cntfrq);

/* Build per-plugin rows from a plugin_time snapshot.  `block_us` is the audio
 * block period in microseconds, `cntfrq` the timer frequency.  Writes up to
 * `cap` rows to `out`, returns the number written, and stores the summed load
 * (per-mille) in *total_load.  Entries with runs == 0 are skipped. */
int prof_build(const pt_entry_t *in, int n, uint32_t block_us, uint64_t cntfrq,
               prof_row_t *out, int cap, uint32_t *total_load);

/* Remaining graph headroom in per-mille (1000 - total_load, floored at 0). */
uint32_t prof_headroom(uint32_t total_load);

/* Render one row as a shell line, e.g.
 *   prof: pid=3 runs=2400 mean=334us max=352us load=33.4% overruns=2
 * Returns characters written (NUL-terminated); `cap` bounds the buffer. */
int prof_render(const prof_row_t *r, char *buf, int cap);

#endif /* ARM64_PROFILER_H */
