/* arch/arm64/mixer.c - mixer / gain-pan / wet-dry routing primitives
 * (Theme D, issue #118) */

#include "mixer.h"

static inline int16_t sat16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void mix_gain(int16_t *dst, const int16_t *src, int32_t gain_q15, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        dst[i] = sat16(((int32_t)src[i] * gain_q15) >> 15);
}

void mix_add(int16_t *acc, const int16_t *src, int32_t gain_q15, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        acc[i] = sat16((int32_t)acc[i] + (((int32_t)src[i] * gain_q15) >> 15));
}

void mix_pan(const int16_t *src, int32_t pan_q15, int16_t *l, int16_t *r, uint32_t n)
{
    int32_t gl = MIX_ONE - pan_q15;
    int32_t gr = pan_q15;
    for (uint32_t i = 0; i < n; i++) {
        l[i] = sat16(((int32_t)src[i] * gl) >> 15);
        r[i] = sat16(((int32_t)src[i] * gr) >> 15);
    }
}

void mix_blend(int16_t *dst, const int16_t *dry, const int16_t *wet, int32_t mix_q15, uint32_t n)
{
    int32_t gd = MIX_ONE - mix_q15;
    for (uint32_t i = 0; i < n; i++)
        dst[i] = sat16(((int32_t)dry[i] * gd + (int32_t)wet[i] * mix_q15) >> 15);
}

void mix_bypass(int16_t *dst, const int16_t *dry, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        dst[i] = dry[i];
}
