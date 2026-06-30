/* arch/arm64/latency.h - audio-callback latency/jitter statistics (Issue #22)
 *
 * The M4 "done when" criterion requires measured latency and jitter numbers.
 * At the start of every audio callback the audio core reads CNTPCT_EL0 and
 * feeds it here; this module keeps a ring of the most recent callbacks and
 * computes min / max / mean / standard deviation of the inter-callback period
 * (the audio cadence) plus a separate IRQ-to-thread wakeup-latency metric.
 *
 * Everything is fixed-point integer math (the kernel builds -mgeneral-regs-
 * only, so no FP on the audio path) and entirely pure, so the statistics are
 * unit-tested on the host (make test-arm-latency).  A reporter on a non-audio
 * core periodically renders a snapshot over UART, so the slow UART write never
 * touches the audio core's timeline.
 */

#ifndef ARM64_LATENCY_H
#define ARM64_LATENCY_H

#include <stdint.h>

/* Number of recent callbacks summarised (the issue asks for the last 1000). */
#define LAT_WINDOW 1000u

typedef struct {
    uint64_t freq;                 /* CNTFRQ_EL0 (counter ticks/second)      */
    uint64_t prev;                 /* previous callback CNTPCT               */
    uint32_t started;              /* 0 until the first callback is seen     */
    uint64_t ring[LAT_WINDOW];     /* recent inter-callback deltas (cycles)  */
    uint32_t n;                    /* valid samples in the ring (<= WINDOW)  */
    uint32_t head;                 /* next write slot                        */
    uint64_t wake_max;             /* worst IRQ->thread wakeup (cycles)      */
    uint64_t wake_sum;             /* sum of wakeup latencies (cycles)       */
    uint64_t wake_n;               /* wakeup samples                         */
} lat_stats_t;

/* A rendered snapshot (all values already converted to microseconds). */
typedef struct {
    uint64_t count;
    uint64_t min_us, max_us, mean_us, stddev_us;
    uint64_t wake_max_us, wake_mean_us;
} lat_summary_t;

/* Initialise with the system-counter frequency (CNTFRQ_EL0). */
void lat_init(lat_stats_t *s, uint64_t cntfrq);

/* Record a callback occurring at absolute counter value `now`.  Returns the
 * delta (cycles) from the previous callback, or 0 on the very first call. */
uint64_t lat_record(lat_stats_t *s, uint64_t now);

/* Record one IRQ-to-thread wakeup-latency sample (cycles from the timer
 * deadline to the first callback instruction). */
void lat_record_wakeup(lat_stats_t *s, uint64_t cycles);

/* Render min/max/mean/stddev of the windowed period and the wakeup metric
 * into `out` (microseconds). */
void lat_summary(const lat_stats_t *s, lat_summary_t *out);

/* Overflow-safe conversion: counter cycles -> microseconds (rounded). */
uint64_t lat_cyc_to_us(uint64_t cycles, uint64_t freq);

/* Integer square root (used for the standard deviation). */
uint64_t lat_isqrt(uint64_t n);

#endif /* ARM64_LATENCY_H */
