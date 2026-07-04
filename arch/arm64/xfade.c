/* arch/arm64/xfade.c - glitch-free crossfade patch switching (Theme A) */

#include "xfade.h"

/* Raised-cosine (Hann) crossfade curve in Q15:
 *
 *     gB[k] = round( XF_ONE * (1 - cos(pi * k / XF_STEPS)) / 2 )
 *
 * The curve is symmetric - gB[XF_STEPS - k] == XF_ONE - gB[k] - so gA + gB is
 * exactly XF_ONE at every step (a constant signal, or two identical patches,
 * survives the mix bit-for-bit), and its slope is zero at both ends, so the
 * fade joins the steady signal on either side without a kink.  Precomputed as
 * integer literals: no floating point at build time or run time. */
static const uint32_t XF_GAIN_B[XF_STEPS + 1u] = {
        0u,   315u,  1247u,  2762u,  4800u,  7282u, 10114u, 13188u,
    16384u, 19580u, 22654u, 25486u, 27968u, 30006u, 31521u, 32453u,
    32768u
};

void xf_init(xf_state_t *s)
{
    s->fading        = 0u;
    s->step          = 0u;
    s->fade_blocks   = 0u;
    s->steady_blocks = 0u;
    s->switches      = 0u;
}

void xf_begin(xf_state_t *s)
{
    if (!s->fading) {
        s->fading = 1u;
        s->step   = 0u;
    }
}

int xf_active(const xf_state_t *s)
{
    return (int)s->fading;
}

uint32_t xf_gain_b(uint32_t step)
{
    return (step > XF_STEPS) ? (uint32_t)XF_ONE : XF_GAIN_B[step];
}

int xf_block(xf_state_t *s, const int16_t *a, const int16_t *b,
             int16_t *dst, uint32_t n)
{
    if (!s->fading) {
        for (uint32_t i = 0u; i < n; i++)
            dst[i] = a[i];
        s->steady_blocks++;
        return 0;
    }

    int32_t gB = (int32_t)XF_GAIN_B[s->step];
    int32_t gA = (int32_t)XF_ONE - gB;

    /* |a|,|b| <= 32767 and gA + gB == 32768, so a*gA + b*gB fits in int32
     * (<= 32767*32768) and the >>15 is an exact divide by XF_ONE. */
    for (uint32_t i = 0u; i < n; i++) {
        int32_t mix = ((int32_t)a[i] * gA + (int32_t)b[i] * gB) >> 15;
        dst[i] = (int16_t)mix;
    }
    s->fade_blocks++;

    if (s->step >= XF_STEPS) {   /* final step: dst == B exactly; latch off */
        s->fading = 0u;
        s->step   = 0u;
        s->switches++;
    } else {
        s->step++;
    }
    return 1;
}
