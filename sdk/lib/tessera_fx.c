/* sdk/lib/tessera_fx.c - reference effects suite for the Tessera SDK (Theme B,
 * issue #111).
 *
 * Complete effects - overdrive, compressor, 3-band EQ, delay, chorus, reverb,
 * noise gate, and a tuner - composed from the DSP primitives in tessera_dsp.c
 * (biquads, oscillators, the fractional delay line, and the envelope follower).
 * Everything here is real-time safe: no libc, no allocation, no unbounded
 * per-sample work.  The few transcendentals (log/pow, needed only for the
 * dB-domain compressor and the tuner's note mapping) are small internal
 * approximations, so the library still needs no libm.
 */

#include "tessera.h"

/* 2^x: floor the integer part, 2^frac by a short polynomial, scale the exponent
 * field (matches the approximation used in tessera_dsp.c). */
static float exp2f_(float x)
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

/* log2(x) for x > 0: pull the exponent from the float bits, then a fast
 * atanh-series for the mantissa in [1,2).  With t = (m-1)/(m+1) (|t| <= 1/3),
 * ln(m) = 2(t + t^3/3 + t^5/5 + t^7/7); through t^7 the error is < 1e-5, so the
 * tuner's cents mapping is accurate to well under a cent. */
static float log2f_(float x)
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

static float powf_(float base, float exp) { return exp2f_(exp * log2f_(base)); }
static float db_to_lin_(float db)         { return exp2f_(db * 0.16609640f); } /* 10^(db/20) */

/* ---- overdrive ----------------------------------------------------------- */

float tessera_fx_overdrive(float x, float drive, float level)
{
    /* Rational tanh approximation: smooth, odd-symmetric, bounded to (-1,1). */
    float u = drive * x;
    if (u >  3.0f) u =  3.0f;                          /* keep the Pade sane   */
    if (u < -3.0f) u = -3.0f;
    float u2 = u * u;
    float t = u * (27.0f + u2) / (27.0f + 9.0f * u2);
    return level * t;
}

/* ---- compressor ---------------------------------------------------------- */

void tessera_fx_comp_init(tessera_fx_comp_t *c, float sr, float atk_ms,
                          float rel_ms, float thresh_db, float ratio,
                          float makeup_db)
{
    tessera_envfollow_init(&c->det, sr, atk_ms, rel_ms);
    c->thresh_lin = db_to_lin_(thresh_db);
    if (ratio < 1.0f) ratio = 1.0f;
    c->inv_ratio_minus_1 = 1.0f / ratio - 1.0f;        /* <= 0 */
    c->makeup_lin = db_to_lin_(makeup_db);
}

float tessera_fx_comp(tessera_fx_comp_t *c, float x)
{
    float env = tessera_envfollow(&c->det, x);
    float gain = 1.0f;
    if (env > c->thresh_lin && env > 0.0f)
        /* out = T*(env/T)^(1/R); gain = out/env = (env/T)^(1/R - 1) */
        gain = powf_(env / c->thresh_lin, c->inv_ratio_minus_1);
    return x * gain * c->makeup_lin;
}

/* ---- 3-band EQ ----------------------------------------------------------- */

void tessera_fx_eq3_init(tessera_fx_eq3_t *eq, float sr,
                         float low_f,  float low_db,
                         float mid_f,  float mid_q, float mid_db,
                         float high_f, float high_db)
{
    tessera_biquad_lowshelf (&eq->low,  sr, low_f,  0.707f, low_db);
    tessera_biquad_peaking  (&eq->mid,  sr, mid_f,  mid_q,  mid_db);
    tessera_biquad_highshelf(&eq->high, sr, high_f, 0.707f, high_db);
}

float tessera_fx_eq3(tessera_fx_eq3_t *eq, float x)
{
    float y = tessera_biquad_process(&eq->low,  x);
    y       = tessera_biquad_process(&eq->mid,  y);
    y       = tessera_biquad_process(&eq->high, y);
    return y;
}

/* ---- delay --------------------------------------------------------------- */

void tessera_fx_delay_init(tessera_fx_delay_t *d, float *buf, uint32_t size)
{
    tessera_delay_init(&d->line, buf, size);
    d->delay_samples = 0.0f;
    d->feedback      = 0.0f;
    d->mix           = 0.0f;
}

void tessera_fx_delay_set(tessera_fx_delay_t *d, float delay_samples,
                          float feedback, float mix)
{
    d->delay_samples = delay_samples;
    d->feedback      = feedback;
    d->mix           = mix;
}

float tessera_fx_delay(tessera_fx_delay_t *d, float x)
{
    float wet = tessera_delay_read(&d->line, d->delay_samples);
    tessera_delay_write(&d->line, x + d->feedback * wet);
    return x * (1.0f - d->mix) + wet * d->mix;
}

/* ---- chorus -------------------------------------------------------------- */

void tessera_fx_chorus_init(tessera_fx_chorus_t *c, float *buf, uint32_t size,
                            float sr, float rate_hz, float base_ms,
                            float depth_ms, float mix)
{
    tessera_delay_init(&c->line, buf, size);
    tessera_osc_set(&c->lfo, sr, rate_hz);
    c->base_samples  = base_ms  * 0.001f * sr;
    c->depth_samples = depth_ms * 0.001f * sr;
    c->mix           = mix;
}

float tessera_fx_chorus(tessera_fx_chorus_t *c, float x)
{
    float lfo = 0.5f * (tessera_osc_sin(&c->lfo) + 1.0f);   /* [0,1] */
    float dly = c->base_samples + c->depth_samples * lfo;
    float wet = tessera_delay_read(&c->line, dly);
    tessera_delay_write(&c->line, x);
    return x * (1.0f - c->mix) + wet * c->mix;
}

/* ---- noise gate ---------------------------------------------------------- */

void tessera_fx_gate_init(tessera_fx_gate_t *g, float sr, float thresh_db,
                          float atk_ms, float rel_ms)
{
    tessera_envfollow_init(&g->det, sr, atk_ms, rel_ms);
    /* The smoother's time constant sets how fast the gate opens/closes. */
    tessera_smooth_init(&g->gain, sr, rel_ms);
    tessera_smooth_set(&g->gain, 0.0f);
    g->thresh_lin = db_to_lin_(thresh_db);
}

float tessera_fx_gate(tessera_fx_gate_t *g, float x)
{
    float env    = tessera_envfollow(&g->det, x);
    float target = env >= g->thresh_lin ? 1.0f : 0.0f;
    float gain   = tessera_smooth(&g->gain, target);
    return x * gain;
}

/* ---- reverb -------------------------------------------------------------- */

void tessera_fx_reverb_init(tessera_fx_reverb_t *r,
                            float *comb_buf[4], uint32_t comb_size[4],
                            float *ap_buf[2],   uint32_t ap_size[2],
                            float feedback, float damp, float mix)
{
    for (int i = 0; i < 4; i++) {
        tessera_delay_init(&r->comb[i], comb_buf[i], comb_size[i]);
        r->comb_lp[i] = 0.0f;
    }
    for (int i = 0; i < 2; i++)
        tessera_delay_init(&r->ap[i], ap_buf[i], ap_size[i]);
    r->feedback = feedback;
    r->damp     = damp;
    r->mix      = mix;
}

float tessera_fx_reverb(tessera_fx_reverb_t *r, float x)
{
    float acc = 0.0f;
    /* Parallel damped feedback combs. */
    for (int i = 0; i < 4; i++) {
        uint32_t d = r->comb[i].size - 1;
        float yD = tessera_delay_read(&r->comb[i], (float)d);
        /* One-pole low-pass in the feedback path = frequency-dependent decay. */
        r->comb_lp[i] = yD * (1.0f - r->damp) + r->comb_lp[i] * r->damp;
        tessera_delay_write(&r->comb[i], x + r->feedback * r->comb_lp[i]);
        acc += yD;
    }
    acc *= 0.25f;
    /* Series allpass diffusers. */
    for (int i = 0; i < 2; i++) {
        const float g = 0.5f;
        uint32_t d = r->ap[i].size - 1;
        float wD = tessera_delay_read(&r->ap[i], (float)d);
        float w  = acc + g * wD;
        tessera_delay_write(&r->ap[i], w);
        acc = wD - g * w;
    }
    return x * (1.0f - r->mix) + acc * r->mix;
}

/* ---- tuner --------------------------------------------------------------- */

void tessera_fx_tuner_init(tessera_fx_tuner_t *t, float sr)
{
    t->sr = sr;
    t->hz = 0.0f;
}

void tessera_fx_tuner_process(tessera_fx_tuner_t *t, const float *x, uint32_t n)
{
    /* Interpolated upward zero-crossings: frequency = crossings-1 spread over
     * the samples between the first and last crossing.  Robust for a clean
     * periodic tone and completely libm-free. */
    float first = -1.0f, last = -1.0f;
    int   count = 0;
    for (uint32_t i = 1; i < n; i++) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            float denom = x[i] - x[i - 1];
            float frac  = denom != 0.0f ? -x[i - 1] / denom : 0.0f;
            float pos   = (float)(i - 1) + frac;
            if (first < 0.0f) first = pos;
            last = pos;
            count++;
        }
    }
    if (count >= 2 && last > first)
        t->hz = (float)(count - 1) * t->sr / (last - first);
    else
        t->hz = 0.0f;
}

float tessera_fx_tuner_hz(const tessera_fx_tuner_t *t) { return t->hz; }

int tessera_fx_note_of(float hz, float *cents)
{
    if (hz <= 0.0f) { if (cents) *cents = 0.0f; return -1; }
    /* MIDI note 69 = A4 = 440 Hz; 12 semitones per octave. */
    float midi = 69.0f + 12.0f * log2f_(hz / 440.0f);
    int   nearest = (int)(midi + (midi >= 0.0f ? 0.5f : -0.5f));  /* round */
    if (cents) *cents = (midi - (float)nearest) * 100.0f;
    return nearest;
}
