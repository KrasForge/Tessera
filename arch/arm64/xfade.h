/* arch/arm64/xfade.h - glitch-free crossfade patch switching
 *                      (Theme A: reliability)
 *
 * Swapping a patch mid-performance must not click.  An abrupt cut from one
 * graph's output to another's leaves a step discontinuity in the waveform - an
 * audible click, the pedal equivalent of a scratchy switch.  This module
 * crossfades: for a short window both the outgoing patch (A) and the incoming
 * patch (B) render, and their DAC-bound blocks are mixed with a gain that ramps
 * A down and B up.  The waveform then moves continuously from A to B and the
 * switch is silent.
 *
 * The ramp is a raised-cosine (Hann) curve: its slope is zero at both ends, so
 * the mix meets the steady-state signal on either side with no kink, and the
 * two gains sum to exactly one (Q15: gA + gB == 32768) at every step, so a
 * constant signal - or two identical patches - passes through the mix
 * unchanged.  Everything is fixed-point Q15 integer arithmetic on the int16 PCM
 * blocks, so the crossfade runs on the -mgeneral-regs-only audio path with no
 * floating point, exactly like the rest of the kernel signal handling.
 *
 * The logic is pure and unit-tested on the host (make test-arm-patch-switch)
 * and demonstrated end to end on QEMU virt (make test-arm-patch-switch-qemu).
 */

#ifndef ARM64_XFADE_H
#define ARM64_XFADE_H

#include <stdint.h>

/* Blocks a crossfade spans.  The ramp visits steps 0..XF_STEPS inclusive, so a
 * fade emits XF_STEPS+1 mixed blocks: the first is exactly A, the last exactly
 * B.  At 48 kHz with a 256-frame block this is ~90 ms - long enough to be
 * click-free, short enough to feel instant. */
#define XF_STEPS   16u

/* Q15 unit gain: gA + gB == XF_ONE at every step. */
#define XF_ONE     32768

typedef struct {
    uint32_t fading;         /* 1 while a crossfade is in progress            */
    uint32_t step;           /* current ramp step, 0..XF_STEPS                */
    uint32_t fade_blocks;    /* blocks emitted as an A/B mix                  */
    uint32_t steady_blocks;  /* blocks emitted straight through (no fade)     */
    uint32_t switches;       /* crossfades completed (B became the new A)     */
} xf_state_t;

/* Reset to the steady, not-fading state. */
void xf_init(xf_state_t *s);

/* Begin a crossfade from the running patch A to a new patch B.  Idempotent
 * while a fade is already in progress (the in-flight fade runs to completion). */
void xf_begin(xf_state_t *s);

/* Non-zero while a crossfade is in progress. */
int xf_active(const xf_state_t *s);

/* B's Q15 gain at ramp step `step` (A's gain is XF_ONE - this).  `step` past
 * XF_STEPS clamps to XF_ONE.  Exposed so the gain-sum invariant is testable. */
uint32_t xf_gain_b(uint32_t step);

/* Produce one output block of `n` int16 samples (interleaved channels are just
 * a flat run of samples here).
 *
 *   - Not fading: copy the running patch A into `dst`.
 *   - Fading:     dst[i] = (A[i]*gA + B[i]*gB) >> 15 with gA + gB == XF_ONE,
 *                 then advance one ramp step.  When the final step completes
 *                 (dst == B exactly) the fade latches off, B becomes the running
 *                 patch, and the caller may retire A.
 *
 * FP-free.  Returns 1 if this block was a mix, 0 if it was a straight copy. */
int xf_block(xf_state_t *s, const int16_t *a, const int16_t *b,
             int16_t *dst, uint32_t n);

#endif /* ARM64_XFADE_H */
