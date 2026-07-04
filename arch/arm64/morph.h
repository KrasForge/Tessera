/* arch/arm64/morph.h - scene / parameter morphing (Theme M17, issue #173)
 *
 * Glitch-free patch switching (issue #103) crossfades the *audio* of two graphs.
 * Scene morphing generalises that to the *parameter* space: interpolate every
 * parameter between two saved snapshots under a single control (an expression
 * pedal, a tempo-synced LFO), so a performer sweeps continuously from one sound
 * to another instead of hard-switching.
 *
 * Each parameter names an interpolation curve: LINEAR (levels, pan), EXP (a
 * geometric sweep for frequencies / times, so an octave is halfway), or STEP (a
 * discrete switch at the midpoint).  A parameter present in only one snapshot
 * holds its value, so mismatched patches morph without error.
 *
 * This is control-plane logic (it runs when the morph control moves, not per
 * audio sample), so - like param_map.h - it is header-only float code, compiled
 * into the host tests and the FP harness, never the -mgeneral-regs-only kernel.
 * The EXP curve uses small self-contained exp2/log2, so it needs no libm.
 */

#ifndef ARM64_MORPH_H
#define ARM64_MORPH_H

#include <stdint.h>

#define MORPH_MAX_PARAMS 64

typedef enum {
    MORPH_LINEAR = 0,   /* v = a + (b-a)*pos                    */
    MORPH_EXP,          /* geometric: v = a*(b/a)^pos (freqs)   */
    MORPH_STEP,         /* v = pos < 0.5 ? a : b                */
} morph_curve_t;

typedef struct {
    uint32_t id;
    float    value;
    uint8_t  curve;     /* morph_curve_t */
} morph_param_t;

typedef struct {
    morph_param_t p[MORPH_MAX_PARAMS];
    int           n;
} morph_snapshot_t;

/* ---- small self-contained exp2 / log2 (no libm) -------------------------- */

static inline float morph_exp2f(float x)
{
    if (x < -126.0f) return 0.0f;
    if (x >  126.0f) x = 126.0f;
    float xi = (float)(int)x;
    if (x < 0.0f && x != xi) xi -= 1.0f;
    float f = x - xi;
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * 0.0555041f));
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)((int)xi + 127) << 23;
    return p * v.f;
}

static inline float morph_log2f(float x)
{
    if (x <= 0.0f) return -126.0f;
    union { float f; uint32_t u; } v = { .f = x };
    int e = (int)((v.u >> 23) & 0xffu) - 127;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;
    float m  = v.f;
    float t  = (m - 1.0f) / (m + 1.0f);
    float t2 = t * t;
    float ln = 2.0f * t * (1.0f + t2 * (1.0f / 3.0f + t2 * (0.2f + t2 * (1.0f / 7.0f))));
    return (float)e + ln * 1.4426950408889634f;
}

/* ---- snapshots ----------------------------------------------------------- */

static inline void morph_init(morph_snapshot_t *s) { s->n = 0; }

static inline int morph_find(const morph_snapshot_t *s, uint32_t id)
{
    for (int i = 0; i < s->n; i++) if (s->p[i].id == id) return i;
    return -1;
}

/* Add or replace a parameter in a snapshot.  Returns 0, or -1 if full. */
static inline int morph_set(morph_snapshot_t *s, uint32_t id, float value, morph_curve_t curve)
{
    int i = morph_find(s, id);
    if (i < 0) {
        if (s->n >= MORPH_MAX_PARAMS) return -1;
        i = s->n++;
        s->p[i].id = id;
    }
    s->p[i].value = value;
    s->p[i].curve = (uint8_t)curve;
    return 0;
}

/* Interpolate two endpoint values under `curve` at position `pos` (0..1). */
static inline float morph_interp(float a, float b, float pos, morph_curve_t curve)
{
    if (pos <= 0.0f) return a;
    if (pos >= 1.0f) return b;
    switch (curve) {
    case MORPH_STEP:
        return pos < 0.5f ? a : b;
    case MORPH_EXP:
        if (a > 0.0f && b > 0.0f)                       /* geometric */
            return a * morph_exp2f(pos * morph_log2f(b / a));
        return a + (b - a) * pos;                       /* fall back to linear */
    case MORPH_LINEAR:
    default:
        return a + (b - a) * pos;
    }
}

/* The morphed value of parameter `id` between snapshots `a` and `b` at `pos`.
 * If the parameter is in both, it is interpolated by `a`'s curve; if in only one,
 * that value is held; if in neither, `dflt` is returned. */
static inline float morph_value(const morph_snapshot_t *a, const morph_snapshot_t *b,
                                uint32_t id, float pos, float dflt)
{
    int ia = morph_find(a, id), ib = morph_find(b, id);
    if (ia >= 0 && ib >= 0)
        return morph_interp(a->p[ia].value, b->p[ib].value, pos, (morph_curve_t)a->p[ia].curve);
    if (ia >= 0) return a->p[ia].value;     /* only in a: hold */
    if (ib >= 0) return b->p[ib].value;     /* only in b: hold */
    return dflt;
}

/* Fill `out` with the morphed value of every parameter in the union of `a` and
 * `b` at `pos`.  Returns the number written (<= max). */
static inline int morph_eval(const morph_snapshot_t *a, const morph_snapshot_t *b,
                             float pos, morph_param_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < a->n && n < max; i++) {
        out[n].id    = a->p[i].id;
        out[n].curve = a->p[i].curve;
        out[n].value = morph_value(a, b, a->p[i].id, pos, a->p[i].value);
        n++;
    }
    /* Parameters only in b. */
    for (int i = 0; i < b->n && n < max; i++) {
        if (morph_find(a, b->p[i].id) >= 0) continue;
        out[n].id    = b->p[i].id;
        out[n].curve = b->p[i].curve;
        out[n].value = b->p[i].value;   /* held (absent in a) */
        n++;
    }
    return n;
}

#endif /* ARM64_MORPH_H */
