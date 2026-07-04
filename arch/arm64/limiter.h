/* arch/arm64/limiter.h - master output limiter / soft-clip (Theme M15, #166)
 *
 * The final stage before the DAC.  A product must not clip its output, so the
 * platform offers a look-ahead peak limiter that a builder can put on the master
 * bus instead of every plugin re-implementing one.
 *
 * The limiter is *look-ahead*: it delays the signal by a short window and, over
 * that window, applies the minimum gain needed so no sample in it can exceed the
 * ceiling.  Because the applied gain is the windowed minimum, the delayed output
 * is provably bounded to +/- ceiling - a true brick wall, no overshoot - while
 * the gain still recovers smoothly (release) between peaks.  A separate soft-clip
 * gives musical saturation for the last dB when a builder wants colour rather
 * than a hard wall.
 *
 * Q15 fixed-point on int16 PCM - runs on the -mgeneral-regs-only audio path.
 * Pure, host-tested (make test-arm-limiter); the caller owns the look-ahead
 * buffers, so the SDK/kernel never allocates.
 */

#ifndef ARM64_LIMITER_H
#define ARM64_LIMITER_H

#include <stdint.h>

typedef struct {
    int16_t *dbuf;       /* look-ahead delay ring (lookahead samples)      */
    int32_t *gbuf;       /* per-slot instantaneous gain, Q15               */
    int      lookahead;  /* window length in samples                       */
    int      w;          /* ring write index                               */
    int32_t  ceiling;    /* output ceiling, int16 amplitude (1..32767)     */
    int32_t  gain;       /* current applied gain, Q15                      */
    int32_t  rel_step;   /* release increment per sample, Q15 (>=1)        */
} limiter_t;

/* Initialise a limiter over caller buffers `dbuf`/`gbuf` (each `lookahead`
 * entries).  `ceiling` is the peak output amplitude (clamped to 1..32767).
 * `release_step_q15` is how much the gain may recover per sample (larger = faster
 * release); it is floored to 1. */
void    limiter_init(limiter_t *l, int16_t *dbuf, int32_t *gbuf, int lookahead,
                     int32_t ceiling, int32_t release_step_q15);

/* Process one sample.  Returns the (look-ahead-delayed) limited sample, provably
 * within +/- ceiling. */
int16_t limiter_process(limiter_t *l, int16_t x);

/* Process a block in place. */
void    limiter_block(limiter_t *l, int16_t *buf, int n);

/* The processing latency in samples (== lookahead). */
int     limiter_latency(const limiter_t *l);

/* Cubic soft-clip of `x` to +/- ceiling: linear well below the ceiling, rounding
 * over smoothly to the ceiling instead of a hard corner.  Stateless. */
int16_t limiter_softclip(int16_t x, int32_t ceiling);

#endif /* ARM64_LIMITER_H */
