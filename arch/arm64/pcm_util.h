/* arch/arm64/pcm_util.h - shared int16 PCM helpers (audio path)
 *
 * Small saturating primitives used across the fixed-point audio modules (mixer,
 * limiter, looper, sample-rate converter).  Header-only inline, integer-only, so
 * each of them stops re-defining the same clamp locally and it still compiles
 * into the -mgeneral-regs-only kernel.
 */

#ifndef ARM64_PCM_UTIL_H
#define ARM64_PCM_UTIL_H

#include <stdint.h>

/* Clamp a 32-bit intermediate to the int16 range (saturating, no wrap). */
static inline int16_t sat16(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

#endif /* ARM64_PCM_UTIL_H */
