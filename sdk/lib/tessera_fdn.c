/* sdk/lib/tessera_fdn.c - feedback-delay-network reverb (Theme M20, issue
 * #191).
 *
 * The effects-suite reverb (#111) is a basic Schroeder topology (4 combs into
 * 2 allpasses) - a fine placeholder, but its short parallel combs ring at
 * audible modal frequencies (the classic metallic tail).  This is the
 * product-grade upgrade: an 8-line FEEDBACK DELAY NETWORK.
 *
 *   - Every line feeds every other through a LOSSLESS Hadamard matrix
 *     (orthogonal, applied as a 24-add fast Walsh-Hadamard butterfly and a
 *     1/sqrt(8) scale), so echo density grows with every pass instead of each
 *     comb ringing alone.
 *   - Decay is exact and per line: g_i = 10^(-3 d_i / (rt60 * sr)), so every
 *     path loses 60 dB in rt60 seconds regardless of its length - the decay
 *     time IS the control, not a side effect of feedback gain.
 *   - Per-line one-pole damping in the feedback path makes highs die faster
 *     than lows, like air.
 *   - Optional slow per-line delay modulation (a few samples, one LFO spread
 *     across the lines) detunes the residual modes so the tail stays smooth.
 *
 * Same contract as the rest of the suite: caller-owned line buffers
 * (mutually-prime lengths recommended), no allocation, no libc, no libm.
 * Stability is structural: the mixing matrix is orthogonal (energy
 * preserving) and every g_i < 1 for any rt60, so the loop is a contraction
 * for the full control range.  tessera_fx_reverb_* stays for the small/cheap
 * case; this is the one to ship.
 */

#include "tessera.h"

/* Exact per-line decay: lose 60 dB over rt60 seconds after repeated trips
 * around a d-sample loop: g = 10^(-3 d / (rt60 sr)) = 2^(-9.9658 d / ...). */
static float rt60_gain(float d_samples, float rt60_s, float sr)
{
    if (rt60_s < 0.05f) rt60_s = 0.05f;
    return tessera_exp2f(-9.965784f * d_samples / (rt60_s * sr));
}

void tessera_fx_reverb2_set(tessera_fx_reverb2_t *r, float rt60_s, float size,
                            float damp, float mix)
{
    r->rt60 = rt60_s;
    r->size = tessera_clampf(size, 0.25f, 1.0f);
    r->damp = tessera_clampf(damp, 0.0f, 0.99f);
    r->mix  = tessera_clampf(mix, 0.0f, 1.0f);
    for (int i = 0; i < TESSERA_FDN_LINES; i++) {
        /* Leave headroom for the modulation excursion + interpolation. */
        float span = (float)(r->line[i].size - 1u) - (r->mod_depth + 2.0f);
        if (span < 8.0f) span = 8.0f;
        r->dlen[i] = span * r->size;
        r->g[i]    = rt60_gain(r->dlen[i], rt60_s, r->sr);
    }
}

void tessera_fx_reverb2_mod(tessera_fx_reverb2_t *r, float rate_hz,
                            float depth_samples)
{
    r->mod_rate  = rate_hz < 0.0f ? 0.0f : rate_hz;
    r->mod_depth = depth_samples < 0.0f ? 0.0f : depth_samples;
    /* Re-derive lengths so the excursion still fits the buffers. */
    tessera_fx_reverb2_set(r, r->rt60, r->size, r->damp, r->mix);
}

void tessera_fx_reverb2_init(tessera_fx_reverb2_t *r, float sr,
                             float *bufs[TESSERA_FDN_LINES],
                             const uint32_t sizes[TESSERA_FDN_LINES],
                             float rt60_s, float size, float damp, float mix)
{
    r->sr = sr;
    r->mod_rate = 0.0f;
    r->mod_depth = 0.0f;
    r->mod_phase = 0.0f;
    for (int i = 0; i < TESSERA_FDN_LINES; i++) {
        tessera_delay_init(&r->line[i], bufs[i], sizes[i]);
        r->lp[i] = 0.0f;
    }
    tessera_fx_reverb2_set(r, rt60_s, size, damp, mix);
}

float tessera_fx_reverb2(tessera_fx_reverb2_t *r, float x)
{
    float y[TESSERA_FDN_LINES];

    /* Read each line at its (possibly modulated) length.  The single slow
     * LFO is spread in phase across the lines so they never move together. */
    float ph = r->mod_phase;
    if (r->mod_rate > 0.0f) {
        r->mod_phase += r->mod_rate / r->sr;
        if (r->mod_phase >= 1.0f)
            r->mod_phase -= 1.0f;
    }
    for (int i = 0; i < TESSERA_FDN_LINES; i++) {
        float d = r->dlen[i];
        if (r->mod_depth > 0.0f)
            d += r->mod_depth *
                 tessera_sinf(TESSERA_TAU * (ph + (float)i * 0.125f));
        y[i] = tessera_delay_read(&r->line[i], d);
    }

    /* Per-line damping + exact decay gain. */
    float v[TESSERA_FDN_LINES];
    for (int i = 0; i < TESSERA_FDN_LINES; i++) {
        r->lp[i] = y[i] * (1.0f - r->damp) + r->lp[i] * r->damp;
        v[i] = r->lp[i] * r->g[i];
    }

    /* Lossless Hadamard mix: fast Walsh-Hadamard butterfly, then 1/sqrt(8). */
    for (int h = 1; h < TESSERA_FDN_LINES; h <<= 1)
        for (int i = 0; i < TESSERA_FDN_LINES; i += h << 1)
            for (int j = i; j < i + h; j++) {
                float a = v[j], b = v[j + h];
                v[j]     = a + b;
                v[j + h] = a - b;
            }
    const float s = 0.35355339f;   /* 1/sqrt(8) */

    /* Feed the mixed returns plus the input back into the lines (input
     * injected with alternating sign for extra decorrelation). */
    float wet = 0.0f;
    for (int i = 0; i < TESSERA_FDN_LINES; i++) {
        float in = (i & 1) ? -x : x;
        tessera_delay_write(&r->line[i], v[i] * s + in * 0.125f);
        wet += (i & 1) ? -y[i] : y[i];
    }
    wet *= 0.5f;

    return x * (1.0f - r->mix) + wet * r->mix;
}
