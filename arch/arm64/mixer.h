/* arch/arm64/mixer.h - mixer / gain-pan / wet-dry routing primitives
 *                      (Theme D, issue #118)
 *
 * Real signal chains need summing, level and pan, a wet/dry blend, and a true
 * bypass.  These are the fixed-point building blocks the mixer, send/return, and
 * gain/pan graph nodes are built from - all Q15 integer math on the int16 PCM
 * blocks, so they run on the -mgeneral-regs-only audio path like the crossfade
 * (#103) they share their blend math with.  Unit-tested on the host
 * (make test-arm-mixer).
 */

#ifndef ARM64_MIXER_H
#define ARM64_MIXER_H

#include <stdint.h>

#define MIX_ONE 32768    /* Q15 unity gain */

/* dst[i] = saturate16( src[i] * gain_q15 >> 15 ).  gain_q15 may exceed MIX_ONE
 * (a boost), in which case the result saturates. */
void mix_gain(int16_t *dst, const int16_t *src, int32_t gain_q15, uint32_t n);

/* Accumulate a source into a bus: acc[i] = saturate16( acc[i] + src*gain>>15 ).
 * Used to sum several inputs into one mix bus. */
void mix_add(int16_t *acc, const int16_t *src, int32_t gain_q15, uint32_t n);

/* Pan a mono source to a stereo pair with a linear (constant-gain) law:
 * pan_q15 == 0 is hard left, MIX_ONE is hard right, MIX_ONE/2 is centre.
 * l[i] = src*(MIX_ONE-pan)>>15 ; r[i] = src*pan>>15. */
void mix_pan(const int16_t *src, int32_t pan_q15, int16_t *l, int16_t *r, uint32_t n);

/* Wet/dry blend (the crossfade mix): dst = dry*(MIX_ONE-mix) + wet*mix >> 15,
 * so mix_q15 == 0 is fully dry and MIX_ONE fully wet, and a constant signal is
 * preserved (the gains sum to unity). */
void mix_blend(int16_t *dst, const int16_t *dry, const int16_t *wet, int32_t mix_q15, uint32_t n);

/* True bypass: dst = dry, bit-for-bit. */
void mix_bypass(int16_t *dst, const int16_t *dry, uint32_t n);

#endif /* ARM64_MIXER_H */
