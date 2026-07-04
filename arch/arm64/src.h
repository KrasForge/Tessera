/* arch/arm64/src.h - sample-rate conversion (Theme H, issue #131)
 *
 * Tessera runs its audio path at a fixed device rate, but sources and sinks may
 * want another: a plugin negotiated for 48 kHz feeding a 96 kHz DAC, a 44.1 kHz
 * file player, USB audio at yet another rate (issue #133).  This is the bridge -
 * a streaming rational resampler that converts an int16 PCM stream from one rate
 * to another.
 *
 * It is fixed-point (a Q32 phase accumulator) and interpolates linearly between
 * input samples, so it needs no floating point and runs on the
 * -mgeneral-regs-only audio path.  Linear interpolation is cheap and exact at DC
 * and at the endpoints; a polyphase-FIR upgrade would improve the stopband but
 * the interface here is unchanged.  Streaming: state carries across blocks, so a
 * long stream resamples block-by-block with no seam.
 *
 * Pure, host-tested (make test-arm-src); no allocation, no libc, no FP.
 */

#ifndef ARM64_SRC_H
#define ARM64_SRC_H

#include <stdint.h>

typedef struct {
    uint32_t in_rate, out_rate;
    uint64_t step;      /* input samples per output sample, Q32          */
    uint64_t frac;      /* current position within [prev, cur), Q32      */
    int32_t  prev;      /* last consumed input sample                    */
    int      primed;    /* prev/frac valid (first input has been taken)  */
} src_t;

/* Initialise a converter from `in_rate` to `out_rate` (Hz, both > 0). */
void src_init(src_t *s, uint32_t in_rate, uint32_t out_rate);

/* Reset the streaming state (rates unchanged), e.g. on a seek or a gap. */
void src_reset(src_t *s);

/* A safe upper bound on the number of output samples `n_in` inputs can yield, so
 * the caller can size the output buffer. */
int  src_out_capacity(const src_t *s, int n_in);

/* Resample `n_in` input samples into `out` (capacity `max_out`).  Returns the
 * number of output samples written.  Streaming: call repeatedly with successive
 * input blocks; the converter remembers its phase across calls. */
int  src_process(src_t *s, const int16_t *in, int n_in, int16_t *out, int max_out);

#endif /* ARM64_SRC_H */
