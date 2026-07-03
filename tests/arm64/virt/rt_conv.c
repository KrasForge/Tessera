/* tests/arm64/virt/rt_conv.c - int16 <-> float conversion for the round-trip
 * loopback harness (Issue #85).
 *
 * The kernel and its harnesses build -mgeneral-regs-only (no FP on the audio
 * path).  The round-trip loop crosses two format boundaries that both need
 * floating point:
 *
 *   - the int16 capture ring (issue #83) -> the plugin's de-interleaved float
 *     page, when the input node hands a captured block to the effect, and
 *   - the plugin's float DAC-output page -> an int16 block, when the modelled
 *     loopback cable feeds the DAC output back into the capture source.
 *
 * Both live in this one translation unit, compiled WITH FP and run at EL1 with
 * FPEN enabled - the stand-in for the DSP-domain conversion a real ADC/DAC
 * adapter would do.  The harness itself stays FP-free and only passes opaque
 * page pointers here.
 */

#include <stdint.h>

/* Convert an interleaved int16 capture block into a plugin's de-interleaved
 * float page: L in page[0..frames), R in page[frames..2*frames). */
void rt_capture_to_page(void *page, const int16_t *interleaved, uint32_t frames)
{
    float *p = (float *)page;
    for (uint32_t f = 0; f < frames; f++) {
        p[f]          = (float)interleaved[2 * f];
        p[frames + f] = (float)interleaved[2 * f + 1];
    }
}

/* Convert a plugin's de-interleaved float DAC-output page back into an
 * interleaved int16 block - the modelled DAC-out-to-ADC-in loopback. */
void rt_page_to_capture(int16_t *interleaved, const void *page, uint32_t frames)
{
    const float *p = (const float *)page;
    for (uint32_t f = 0; f < frames; f++) {
        interleaved[2 * f]     = (int16_t)p[f];
        interleaved[2 * f + 1] = (int16_t)p[frames + f];
    }
}
