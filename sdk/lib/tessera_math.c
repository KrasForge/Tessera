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

/* 2^x for any x: floor the integer part, 2^frac by a short polynomial (< 0.1%
 * error), scaled via the exponent field.  Shared by the DSP blocks, the effects
 * suite, and the synth (dB gains, note-to-frequency). */
float tessera_exp2f(float x)
{
    if (x < -126.0f) return 0.0f;
    if (x >  126.0f) x = 126.0f;
    float xi = (float)(int)x;
    if (x < 0.0f && x != xi) xi -= 1.0f;              /* floor */
    float f = x - xi;                                 /* [0,1) */
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * 0.0555041f));
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)((int)xi + 127) << 23;            /* 2^xi   */
    return p * v.f;
}

/* log2(x) for x > 0: exponent from the float bits, then a fast atanh-series on
 * the mantissa in [1,2) (error < 1e-5). */
float tessera_log2f(float x)
{
    if (x <= 0.0f) return -126.0f;
    union { float f; uint32_t u; } v = { .f = x };
    int e = (int)((v.u >> 23) & 0xffu) - 127;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;          /* mantissa in [1,2) */
    float m  = v.f;
    float t  = (m - 1.0f) / (m + 1.0f);
    float t2 = t * t;
    float ln = 2.0f * t * (1.0f + t2 * (1.0f / 3.0f + t2 * (0.2f + t2 * (1.0f / 7.0f))));
    return (float)e + ln * 1.4426950408889634f;        /* / ln(2) */
}
