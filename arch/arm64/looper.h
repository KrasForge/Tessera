/* arch/arm64/looper.h - multi-track loop pedal (Theme M17, issue #172)
 *
 * A looper records a phrase and plays it back forever, then overdubs more layers
 * on top - the defining live-performance pedal.  Two things it does that a
 * single-process host cannot promise: the loop buffers are the caller's fixed
 * memory (so a runaway record can never exhaust system RAM - it is bounded by the
 * per-plugin memory quota), and record/overdub start and stop are quantized to
 * the transport grid (issue #114) so layers stay locked to the bar.
 *
 * Transitions are click-free: the punch-in / punch-out edges of each recorded
 * layer are short-ramped, so the loop seam and every overdub boundary are
 * silence-to-signal rather than a step.
 *
 * Q15 fixed-point on int16 PCM - runs on the -mgeneral-regs-only audio path.
 * Pure, host-tested (make test-arm-looper); the caller owns the track buffers.
 */

#ifndef ARM64_LOOPER_H
#define ARM64_LOOPER_H

#include <stdint.h>

#define LOOPER_MAX_TRACKS 8
#define LOOPER_DECLICK    64   /* edge-ramp length in samples */

typedef enum {
    LOOP_IDLE = 0,   /* empty, input passes through          */
    LOOP_RECORDING,  /* laying down the first layer (sets loop length) */
    LOOP_PLAYING,    /* looping the recorded layers          */
    LOOP_OVERDUB,    /* looping AND recording a new layer    */
} loop_state_t;

typedef struct {
    int16_t *track[LOOPER_MAX_TRACKS];  /* caller-owned track buffers */
    int      n_tracks;                  /* buffers provided           */
    uint32_t cap;                       /* samples per track (memory bound) */
    uint32_t quantum;                   /* grid size in samples (e.g. one bar) */

    loop_state_t state;
    uint32_t loop_len;                  /* samples in the loop (0 until first record) */
    uint32_t pos;                       /* play / record cursor       */
    int      layers;                    /* recorded layers so far     */
    int      cur;                       /* track being (over)dubbed    */
} looper_t;

/* Initialise over `n_tracks` caller buffers of `cap` samples each.  `quantum` is
 * the quantise grid in samples (record length snaps to a multiple of it); pass 1
 * for no quantisation. */
void looper_init(looper_t *l, int16_t *const *tracks, int n_tracks,
                 uint32_t cap, uint32_t quantum);

/* Toggle record: from IDLE it starts the first layer; from PLAYING it starts an
 * overdub on the next free track.  Returns the new state. */
loop_state_t looper_record(looper_t *l);

/* Stop the current record/overdub: snaps the loop length to the quantise grid on
 * the first layer, ramps the layer's edges click-free, and returns to PLAYING. */
loop_state_t looper_stop(looper_t *l);

/* Clear everything back to empty. */
void looper_clear(looper_t *l);

/* Process one input sample, returning the looper's output (recorded layers mixed,
 * plus input monitoring while recording).  Advances the cursor. */
int16_t looper_process(looper_t *l, int16_t in);

#endif /* ARM64_LOOPER_H */
