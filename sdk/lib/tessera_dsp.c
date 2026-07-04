/* sdk/lib/tessera_dsp.c - DSP building blocks for the Tessera SDK (Theme B).
 *
 * The blocks every effect and synth needs - one-pole smoothers, RBJ biquads,
 * oscillators (with polyBLEP anti-aliasing), a fractional delay line, an
 * envelope follower, and an ADSR - so plugin authors do not start from sinf and
 * a bare buffer.  All real-time safe: no libc, no allocation, no unbounded
 * per-sample work.  Trig uses the SDK's tessera_sinf; the few transcendentals
 * (sqrt, 2^x) are small internal approximations so the library needs no libm.
 */

#include "tessera.h"

#define PI_F   3.14159265358979323846f
#define HALFPI 1.57079632679489661923f

static float cosf_(float x)       { return tessera_sinf(x + HALFPI); }
static float absf_(float x)       { return x < 0.0f ? -x : x; }

/* sqrt via one bit-hack seed + Newton steps (positive inputs only). */
static float sqrtf_(float x)
{
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t u; } v = { .f = x };
    v.u = 0x1fbd1df5u + (v.u >> 1);               /* rough seed */
    float y = v.f;
    for (int i = 0; i < 4; i++) y = 0.5f * (y + x / y);
    return y;
}

/* 2^x for x in any range: split integer and fractional parts, 2^frac by a
 * 3rd-order minimax-ish polynomial (< 0.1% error) scaled via the exponent. */
static float exp2f_(float x)
{
    if (x < -126.0f) return 0.0f;
    if (x >  126.0f) x = 126.0f;
    float xi = (float)(int)x;
    if (x < 0.0f && x != xi) xi -= 1.0f;          /* floor */
    float f = x - xi;                             /* [0,1) */
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * 0.0555041f));
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)((int)xi + 127) << 23;        /* 2^xi   */
    return p * v.f;
}
static float pow10_(float x) { return exp2f_(x * 3.32192809f); }  /* 10^x */

/* ---- one-pole parameter smoother ---------------------------------------- */

void tessera_smooth_init(tessera_smooth_t *s, float sr, float time_ms)
{
    float tau = time_ms * 0.001f * sr;            /* time constant in samples */
    if (tau < 0.0f) tau = 0.0f;
    s->a = tau / (tau + 1.0f);                    /* pole; exp-free, stable   */
    s->y = 0.0f;
}
void tessera_smooth_set(tessera_smooth_t *s, float value) { s->y = value; }
float tessera_smooth(tessera_smooth_t *s, float target)
{
    s->y = s->a * s->y + (1.0f - s->a) * target;
    return s->y;
}

/* ---- RBJ biquad ---------------------------------------------------------- */

static void biquad_norm(tessera_biquad_t *bq, float b0, float b1, float b2,
                        float a0, float a1, float a2)
{
    float inv = 1.0f / a0;
    bq->b0 = b0 * inv; bq->b1 = b1 * inv; bq->b2 = b2 * inv;
    bq->a1 = a1 * inv; bq->a2 = a2 * inv;
}
static void wq(float sr, float f, float q, float *w0, float *cw, float *alpha)
{
    *w0 = TESSERA_TAU * f / sr;
    *cw = cosf_(*w0);
    float sw = tessera_sinf(*w0);
    if (q < 0.0001f) q = 0.0001f;
    *alpha = sw / (2.0f * q);
}

void tessera_biquad_lowpass(tessera_biquad_t *bq, float sr, float f, float q)
{
    float w0, cw, a; wq(sr, f, q, &w0, &cw, &a);
    biquad_norm(bq, (1-cw)*0.5f, 1-cw, (1-cw)*0.5f, 1+a, -2*cw, 1-a);
    tessera_biquad_reset(bq);
}
void tessera_biquad_highpass(tessera_biquad_t *bq, float sr, float f, float q)
{
    float w0, cw, a; wq(sr, f, q, &w0, &cw, &a);
    biquad_norm(bq, (1+cw)*0.5f, -(1+cw), (1+cw)*0.5f, 1+a, -2*cw, 1-a);
    tessera_biquad_reset(bq);
}
void tessera_biquad_bandpass(tessera_biquad_t *bq, float sr, float f, float q)
{
    float w0, cw, a; wq(sr, f, q, &w0, &cw, &a);
    biquad_norm(bq, a, 0.0f, -a, 1+a, -2*cw, 1-a);
    tessera_biquad_reset(bq);
}
void tessera_biquad_notch(tessera_biquad_t *bq, float sr, float f, float q)
{
    float w0, cw, a; wq(sr, f, q, &w0, &cw, &a);
    biquad_norm(bq, 1.0f, -2*cw, 1.0f, 1+a, -2*cw, 1-a);
    tessera_biquad_reset(bq);
}
void tessera_biquad_peaking(tessera_biquad_t *bq, float sr, float f, float q, float gain_db)
{
    float w0, cw, a; wq(sr, f, q, &w0, &cw, &a);
    float A = pow10_(gain_db / 40.0f);
    biquad_norm(bq, 1+a*A, -2*cw, 1-a*A, 1+a/A, -2*cw, 1-a/A);
    tessera_biquad_reset(bq);
}
void tessera_biquad_lowshelf(tessera_biquad_t *bq, float sr, float f, float q, float gain_db)
{
    float w0, cw, a; wq(sr, f, q, &w0, &cw, &a);
    float A = pow10_(gain_db / 40.0f);
    float b = 2.0f * sqrtf_(A) * a;
    biquad_norm(bq,
        A*((A+1)-(A-1)*cw + b), 2*A*((A-1)-(A+1)*cw), A*((A+1)-(A-1)*cw - b),
        (A+1)+(A-1)*cw + b,    -2*((A-1)+(A+1)*cw),  (A+1)+(A-1)*cw - b);
    tessera_biquad_reset(bq);
}
void tessera_biquad_highshelf(tessera_biquad_t *bq, float sr, float f, float q, float gain_db)
{
    float w0, cw, a; wq(sr, f, q, &w0, &cw, &a);
    float A = pow10_(gain_db / 40.0f);
    float b = 2.0f * sqrtf_(A) * a;
    biquad_norm(bq,
        A*((A+1)+(A-1)*cw + b), -2*A*((A-1)+(A+1)*cw), A*((A+1)+(A-1)*cw - b),
        (A+1)-(A-1)*cw + b,     2*((A-1)-(A+1)*cw),   (A+1)-(A-1)*cw - b);
    tessera_biquad_reset(bq);
}
void tessera_biquad_reset(tessera_biquad_t *bq) { bq->z1 = bq->z2 = 0.0f; }

float tessera_biquad_process(tessera_biquad_t *bq, float x)
{
    /* transposed direct form II */
    float y = bq->b0 * x + bq->z1;
    bq->z1 = bq->b1 * x - bq->a1 * y + bq->z2;
    bq->z2 = bq->b2 * x - bq->a2 * y;
    return y;
}

/* ---- oscillator ---------------------------------------------------------- */

void tessera_osc_set(tessera_osc_t *o, float sr, float freq)
{
    o->inc = freq / sr;                           /* normalised phase step */
    if (o->inc < 0.0f) o->inc = 0.0f;
}
static void osc_advance(tessera_osc_t *o)
{
    o->phase += o->inc;
    if (o->phase >= 1.0f) o->phase -= 1.0f;
}
/* polyBLEP residual at normalised phase t with step dt (removes the step's
 * highest aliases cheaply). */
static float polyblep(float t, float dt)
{
    if (dt <= 0.0f) return 0.0f;
    if (t < dt)        { t /= dt;        return t + t - t*t - 1.0f; }
    if (t > 1.0f - dt) { t = (t-1.0f)/dt; return t*t + t + t + 1.0f; }
    return 0.0f;
}
float tessera_osc_sin(tessera_osc_t *o)
{
    float y = tessera_sinf(o->phase * TESSERA_TAU);
    osc_advance(o);
    return y;
}
float tessera_osc_saw(tessera_osc_t *o)
{
    float y = 2.0f * o->phase - 1.0f;
    y -= polyblep(o->phase, o->inc);
    osc_advance(o);
    return y;
}
float tessera_osc_square(tessera_osc_t *o)
{
    float y = o->phase < 0.5f ? 1.0f : -1.0f;
    y += polyblep(o->phase, o->inc);
    float t2 = o->phase + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
    y -= polyblep(t2, o->inc);
    osc_advance(o);
    return y;
}
float tessera_osc_triangle(tessera_osc_t *o)
{
    float y = o->phase < 0.5f ? (4.0f * o->phase - 1.0f)
                              : (3.0f - 4.0f * o->phase);
    osc_advance(o);
    return y;
}

/* ---- fractional delay line ---------------------------------------------- */

void tessera_delay_init(tessera_delay_t *d, float *buf, uint32_t size)
{
    d->buf = buf; d->size = size; d->w = 0;
    for (uint32_t i = 0; i < size; i++) buf[i] = 0.0f;
}
void tessera_delay_write(tessera_delay_t *d, float x)
{
    d->buf[d->w] = x;
    d->w = (d->w + 1u) % d->size;
}
float tessera_delay_read(const tessera_delay_t *d, float delay_samples)
{
    if (delay_samples < 0.0f) delay_samples = 0.0f;
    float maxd = (float)(d->size - 1u);
    if (delay_samples > maxd) delay_samples = maxd;
    uint32_t di = (uint32_t)delay_samples;
    float frac = delay_samples - (float)di;
    /* most-recent sample is at (w-1); read back `delay` from there */
    uint32_t i0 = (d->w + d->size - 1u - di) % d->size;
    uint32_t i1 = (i0 + d->size - 1u) % d->size;
    return d->buf[i0] * (1.0f - frac) + d->buf[i1] * frac;
}
float tessera_delay_tick(tessera_delay_t *d, float x, float delay_samples)
{
    tessera_delay_write(d, x);
    return tessera_delay_read(d, delay_samples);
}

/* ---- envelope follower --------------------------------------------------- */

static float onepole_coeff(float sr, float ms)
{
    float tau = ms * 0.001f * sr;
    return tau < 1.0f ? 0.0f : tau / (tau + 1.0f);
}
void tessera_envfollow_init(tessera_envfollow_t *e, float sr, float atk_ms, float rel_ms)
{
    e->atk = onepole_coeff(sr, atk_ms);
    e->rel = onepole_coeff(sr, rel_ms);
    e->env = 0.0f;
}
float tessera_envfollow(tessera_envfollow_t *e, float x)
{
    float r = absf_(x);
    float c = r > e->env ? e->atk : e->rel;
    e->env = c * e->env + (1.0f - c) * r;
    return e->env;
}

/* ---- ADSR ---------------------------------------------------------------- */

void tessera_adsr_init(tessera_adsr_t *e, float sr, float a_ms, float d_ms,
                       float sustain, float r_ms)
{
    float as = a_ms * 0.001f * sr, ds = d_ms * 0.001f * sr, rs = r_ms * 0.001f * sr;
    e->a_rate = as < 1.0f ? 1.0f : 1.0f / as;
    e->d_rate = ds < 1.0f ? 1.0f : 1.0f / ds;
    e->r_rate = rs < 1.0f ? 1.0f : 1.0f / rs;
    e->sustain = tessera_clampf(sustain, 0.0f, 1.0f);
    e->level = 0.0f;
    e->stage = TESSERA_ADSR_IDLE;
}
void tessera_adsr_gate(tessera_adsr_t *e, int on)
{
    e->stage = on ? TESSERA_ADSR_ATTACK : TESSERA_ADSR_RELEASE;
}
float tessera_adsr(tessera_adsr_t *e)
{
    switch (e->stage) {
    case TESSERA_ADSR_ATTACK:
        e->level += e->a_rate;
        if (e->level >= 1.0f) { e->level = 1.0f; e->stage = TESSERA_ADSR_DECAY; }
        break;
    case TESSERA_ADSR_DECAY:
        e->level -= e->d_rate;
        if (e->level <= e->sustain) { e->level = e->sustain; e->stage = TESSERA_ADSR_SUSTAIN; }
        break;
    case TESSERA_ADSR_SUSTAIN:
        e->level = e->sustain;
        break;
    case TESSERA_ADSR_RELEASE:
        e->level -= e->r_rate;
        if (e->level <= 0.0f) { e->level = 0.0f; e->stage = TESSERA_ADSR_IDLE; }
        break;
    default:
        e->level = 0.0f;
        break;
    }
    return e->level;
}
