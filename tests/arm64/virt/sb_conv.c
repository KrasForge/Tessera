/* tests/arm64/virt/sb_conv.c - test-tone fill for the safe-mode bypass demo
 * (Theme A).
 *
 * The harness builds -mgeneral-regs-only (no FP on the audio path), but the
 * effect plugin expects real float samples at its input.  Producing them needs
 * floating point, so it lives in this one translation unit, compiled WITH FP;
 * the harness stays FP-free and only passes an opaque page pointer here.
 */

#include <stdint.h>

/* Fill a de-interleaved stereo float page with a fixed, non-DC test tone:
 * L in page[0..frames), R in page[frames..2*frames).  The value stays strictly
 * positive (never silent) but varies per sample, so a low-pass filter visibly
 * alters it - which lets the harness tell "signal from the live effect" from
 * "dry input passed through by the bypass". */
void sb_fill_tone(void *page, uint32_t frames)
{
    float *p = (float *)page;
    for (uint32_t f = 0; f < frames; f++) {
        float v = 0.40f + 0.05f * (float)((int)(f % 8u) - 4);   /* 0.20 .. 0.55 */
        p[f]          = v;
        p[frames + f] = v;
    }
}
