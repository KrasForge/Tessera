/* sdk/lib/tessera_math.c - real-time-safe DSP math for the Tessera SDK
 * (Issue #38).  No libc, no allocation. */

#include "tessera.h"

static float absf(float x) { return x < 0.0f ? -x : x; }

float tessera_sinf(float x)
{
    /* Range-reduce x to [-pi, pi] by subtracting the nearest multiple of 2*pi. */
    const float inv_tau = 0.15915494309189535f;   /* 1 / (2*pi) */
    float k  = x * inv_tau;
    int   ki = (int)(k + (k >= 0.0f ? 0.5f : -0.5f));   /* round to nearest */
    x -= (float)ki * TESSERA_TAU;

    /* Classic fast sine on [-pi, pi]: a parabola plus one refinement step,
     * giving < 0.1% peak error (Bhaskara base with the "extra precision" Q/P
     * correction). */
    const float B = 1.2732395447351628f;    /*  4 / pi        */
    const float C = -0.40528473456935109f;  /* -4 / (pi*pi)   */
    float y = B * x + C * x * absf(x);

    const float P = 0.225f;
    y = P * (y * absf(y) - y) + y;
    return y;
}

float tessera_clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}
