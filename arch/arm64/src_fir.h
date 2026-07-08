/* arch/arm64/src_fir.h - polyphase windowed-sinc sample-rate converter
 * (Theme M20, issue #192)
 *
 * The linear-interpolation SRC (src.h, issue #131) is cheap and exact at DC,
 * but it aliases on large ratios and rolls off the top octave.  This is the
 * product-grade upgrade: a windowed-sinc prototype split into polyphase
 * sub-filters - flat passband, a real stopband, and an anti-alias cutoff that
 * scales with the ratio when downsampling.  Same streaming interface shape as
 * src_t.
 *
 * Design (all integer, generated at init - no floating point anywhere, so it
 * builds and runs under the kernel's -mgeneral-regs-only):
 *   - prototype: Blackman-windowed sinc, SRC_FIR_TAPS input samples wide,
 *     sampled at SRC_FIR_PHASES sub-positions per input sample;
 *   - cutoff: 0.9 * min(1, out_rate/in_rate) of the input Nyquist, so
 *     downsampling rejects would-be aliases and upsampling rejects images;
 *   - per-output: the fractional position (the same Q32 accumulator as the
 *     linear SRC) selects two adjacent phases, their coefficients are
 *     linearly interpolated (Q15), and SRC_FIR_TAPS MACs produce the sample;
 *   - every phase's taps are normalised to sum to exactly 32768, so DC is
 *     bit-exact (matching the linear SRC's DC behaviour) - and any
 *     interpolated mix of two phases sums to 32768 too;
 *   - the sinc/window generation uses a self-contained Q15 integer sine
 *     (Bhaskara), at INIT time only.
 *
 * Streaming: history and position live in the struct, so feeding the same
 * samples in any chunking yields bit-identical output.  The filter is centred,
 * so output lags input by SRC_FIR_TAPS/2 samples (constant group delay).
 * Host-tested: make test-arm-src-fir.
 */

#ifndef ARM64_SRC_FIR_H
#define ARM64_SRC_FIR_H

#include <stdint.h>

#define SRC_FIR_TAPS   32u   /* prototype width, input samples (power of 2) */
#define SRC_FIR_PHASES 32u   /* sub-positions per input sample              */

typedef struct {
    uint32_t in_rate, out_rate;
    uint64_t step;      /* input samples per output sample, Q32            */
    uint64_t pos;       /* next output's position in input samples, Q32    */
    uint64_t n_in;      /* input samples consumed                          */
    int16_t  hist[SRC_FIR_TAPS];   /* ring of the last TAPS input samples  */
    /* PHASES+1 sub-filters so phase interpolation can read q and q+1. */
    int16_t  coef[SRC_FIR_PHASES + 1u][SRC_FIR_TAPS];
} src_fir_t;

/* Configure for in_rate -> out_rate and build the polyphase table (integer
 * math; init-time only - keep it off the audio path). */
void src_fir_init(src_fir_t *s, uint32_t in_rate, uint32_t out_rate);

/* Reset the streaming state (keeps the coefficient table). */
void src_fir_reset(src_fir_t *s);

/* Upper bound on outputs produced for n_in inputs (same contract as
 * src_out_capacity). */
int src_fir_out_capacity(const src_fir_t *s, int n_in);

/* Consume n_in samples, produce output samples (up to max_out are written to
 * `out`; the return value counts all produced, like src_process). */
int src_fir_process(src_fir_t *s, const int16_t *in, int n_in,
                    int16_t *out, int max_out);

#endif /* ARM64_SRC_FIR_H */
