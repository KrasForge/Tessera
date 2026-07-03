/* tests/arm64/virt/gi_conv.c - int16 <-> float conversion for the input-node
 * harness (Issue #84).
 *
 * The kernel and its harnesses build -mgeneral-regs-only (no FP on the audio
 * path).  Bridging the int16 capture ring to the float plugin format needs
 * floating point, so it lives in this one translation unit, compiled WITH FP
 * and run at EL1 with FPEN enabled - the stand-in for the DSP-domain
 * conversion a real input adapter would do.  The harness itself stays FP-free
 * and only passes opaque page pointers here.
 */

#include <stdint.h>

/* Convert an interleaved int16 capture block into a plugin's de-interleaved
 * float page: L in page[0..frames), R in page[frames..2*frames). */
void gi_capture_to_page(void *page, const int16_t *interleaved, uint32_t frames)
{
    float *p = (float *)page;
    for (uint32_t f = 0; f < frames; f++) {
        p[f]          = (float)interleaved[2 * f];
        p[frames + f] = (float)interleaved[2 * f + 1];
    }
}

/* Cast a de-interleaved float page back to int16 and compare, frame by frame,
 * against an interleaved capture block.  Returns 1 if every sample matches. */
int gi_page_matches_capture(const void *page, const int16_t *interleaved,
                            uint32_t frames)
{
    const float *p = (const float *)page;
    for (uint32_t f = 0; f < frames; f++) {
        if ((int16_t)p[f]          != interleaved[2 * f])     return 0;
        if ((int16_t)p[frames + f] != interleaved[2 * f + 1]) return 0;
    }
    return 1;
}
