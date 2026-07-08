/* sdk/lib/tessera_spectrum.c - spectrum analyser and FFT tuner for the
 * Tessera SDK (Theme M18, issue #187).
 *
 * The platform's first look INTO the audio: the OLED UI (issue #121) shows
 * CPU and headroom, but nothing about the signal.  Two analysis blocks over
 * the FFT primitive (#184) fix that:
 *
 *   - tessera_spectrum_*: windowed rFFT magnitude, binned into a small number
 *     of LOG-FREQUENCY bars (musical octaves are equal-width), each mapped to
 *     a 0..1000 per-mille dB level (-60 dB .. 0 dBFS) with per-bar peak-hold
 *     and decay - exactly the integers the OLED spectrum screen renders.
 *
 *   - tessera_ftuner_*: an FFT tuner far more robust than the zero-crossing
 *     estimator (issue #111): the fundamental is the strongest spectral peak
 *     (parabolic interpolation for the coarse position), REFINED by the phase
 *     advance of that bin across a hop - the same instantaneous-frequency
 *     math as the phase vocoder - which resolves the frequency to a small
 *     fraction of a bin.  Broadband noise spreads over all bins while the
 *     tone stays concentrated in one, so peak picking shrugs off noise that
 *     derails zero crossings.  Feed the result to tessera_fx_note_of for the
 *     note name and cents.
 *
 * The kernel side stays integer-only: these blocks produce per-mille levels
 * and Hz*10 integers, and arch/arm64/oled_ui.c renders them.  Caller-owned
 * buffers, no allocation, no libc, no libm (magnitude/phase math from
 * tessera_math.c).
 */

#include "tessera.h"

/* dB of a squared magnitude, relative to a squared reference:
 * 10*log10(m2/ref2) = 3.0103 * log2(m2/ref2).  Squared inputs avoid sqrt. */
static float db_of_sq(float m2, float ref2)
{
    if (m2 <= 0.0f)
        return -120.0f;
    return 3.0102999566f * (tessera_log2f(m2) - tessera_log2f(ref2));
}

/* ---- spectrum analyser ---------------------------------------------------- */

uint32_t tessera_spectrum_floats(uint32_t n) { return 2u * n; }
uint32_t tessera_spectrum_u32(uint32_t nbars) { return 3u * nbars + 1u; }

int tessera_spectrum_init(tessera_spectrum_t *sp, uint32_t n, float sr,
                          uint32_t nbars, uint32_t decay,
                          const tessera_cpx_t *tw,
                          float *mem, tessera_cpx_t *cmem, uint32_t *umem)
{
    if (!sp || !tw || !mem || !cmem || !umem)
        return -1;
    if (n < 64u || (n & (n - 1u)) != 0u || nbars < 2u || sr <= 0.0f)
        return -1;

    sp->n     = n;
    sp->nbars = nbars;
    sp->sr    = sr;
    sp->decay = decay;
    sp->tw    = tw;
    sp->win   = mem;
    sp->frame = mem + n;
    sp->spec  = cmem;
    sp->edges = umem;
    sp->bars  = umem + (nbars + 1u);
    sp->peaks = umem + (nbars + 1u) + nbars;

    tessera_window_hann(sp->win, n);

    /* Log-spaced bar edges from ~50 Hz to Nyquist, as rFFT bin indices.
     * edge(i) = f_lo * (f_hi/f_lo)^(i/nbars), forced strictly increasing so
     * every bar owns at least one bin. */
    float f_lo = 50.0f;
    float f_hi = sr * 0.5f;
    float span = tessera_log2f(f_hi / f_lo);
    for (uint32_t i = 0; i <= nbars; i++) {
        float f   = f_lo * tessera_exp2f(span * (float)i / (float)nbars);
        uint32_t b = (uint32_t)(f * (float)n / sr + 0.5f);
        if (i > 0u && b <= sp->edges[i - 1u])
            b = sp->edges[i - 1u] + 1u;
        if (b > n / 2u)
            b = n / 2u;
        sp->edges[i] = b;
    }

    for (uint32_t i = 0; i < nbars; i++) {
        sp->bars[i]  = 0;
        sp->peaks[i] = 0;
    }
    return 0;
}

void tessera_spectrum_process(tessera_spectrum_t *sp, const float *x)
{
    uint32_t n = sp->n;

    for (uint32_t i = 0; i < n; i++)
        sp->frame[i] = x[i] * sp->win[i];
    tessera_rfft(sp->frame, sp->spec, sp->tw, n);

    /* Full-scale reference: an amplitude-1 tone at an exact bin, Hann
     * windowed, lands |X| = n/4 (coherent gain 0.5 of the n/2 line). */
    float ref  = (float)n * 0.25f;
    float ref2 = ref * ref;

    for (uint32_t b = 0; b < sp->nbars; b++) {
        float m2max = 0.0f;
        for (uint32_t k = sp->edges[b]; k < sp->edges[b + 1u]; k++) {
            float m2 = sp->spec[k].re * sp->spec[k].re +
                       sp->spec[k].im * sp->spec[k].im;
            if (m2 > m2max) m2max = m2;
        }
        /* -60 dB .. 0 dBFS -> 0 .. 1000 per-mille. */
        float db = db_of_sq(m2max, ref2);
        float lv = (db + 60.0f) * (1000.0f / 60.0f);
        uint32_t level = lv <= 0.0f ? 0u
                       : lv >= 1000.0f ? 1000u : (uint32_t)lv;
        sp->bars[b] = level;

        /* Peak-hold with linear decay. */
        uint32_t held = sp->peaks[b] > sp->decay ? sp->peaks[b] - sp->decay : 0u;
        sp->peaks[b] = level > held ? level : held;
    }
}

/* ---- FFT tuner ------------------------------------------------------------ */

uint32_t tessera_ftuner_floats(uint32_t n) { return 3u * n + (n / 2u + 1u); }

int tessera_ftuner_init(tessera_ftuner_t *t, uint32_t n, float sr,
                        const tessera_cpx_t *tw, float *mem, tessera_cpx_t *cmem)
{
    if (!t || !tw || !mem || !cmem)
        return -1;
    if (n < 256u || (n & (n - 1u)) != 0u || sr <= 0.0f)
        return -1;

    t->n     = n;
    t->hop   = n / 4u;
    t->sr    = sr;
    t->tw    = tw;
    t->win   = mem;
    t->ring  = mem + n;
    t->frame = mem + 2u * n;
    t->phase = mem + 3u * n;             /* bins entries */
    t->spec  = cmem;

    tessera_window_hann(t->win, n);
    tessera_ftuner_reset(t);
    return 0;
}

void tessera_ftuner_reset(tessera_ftuner_t *t)
{
    for (uint32_t i = 0; i < t->n; i++)
        t->ring[i] = 0.0f;
    for (uint32_t k = 0; k < t->n / 2u + 1u; k++)
        t->phase[k] = 0.0f;
    t->wpos   = 0;
    t->fill   = 0;
    t->primed = 0;
    t->hz     = 0.0f;
}

/* Analyse the current ring (called every hop). */
static void ftuner_analyse(tessera_ftuner_t *t)
{
    uint32_t n = t->n, bins = n / 2u + 1u;

    /* Materialise the circular ring in time order (oldest first) + window. */
    for (uint32_t i = 0; i < n; i++)
        t->frame[i] = t->ring[(t->wpos + i) & (n - 1u)] * t->win[i];
    tessera_rfft(t->frame, t->spec, t->tw, n);

    /* Strongest interior bin (squared magnitudes; no sqrt needed). */
    uint32_t peak = 0;
    float    pm2  = 0.0f;
    for (uint32_t k = 1; k + 1u < bins; k++) {
        float m2 = t->spec[k].re * t->spec[k].re +
                   t->spec[k].im * t->spec[k].im;
        if (m2 > pm2) { pm2 = m2; peak = k; }
    }

    /* Silence gate: require the peak to stand clearly above the frame's mean
     * bin power, and above an absolute floor. */
    float total = 0.0f;
    for (uint32_t k = 1; k + 1u < bins; k++)
        total += t->spec[k].re * t->spec[k].re +
                 t->spec[k].im * t->spec[k].im;
    float mean = total / (float)(bins - 2u);
    float ref  = (float)n * 0.25f;
    if (peak == 0u || pm2 < 1e-8f * ref * ref || pm2 < 20.0f * mean) {
        t->primed = 0;                   /* lost the tone; re-prime the phase */
        t->hz = 0.0f;
        return;
    }

    float ph = tessera_atan2f(t->spec[peak].im, t->spec[peak].re);

    if (t->primed) {
        /* Instantaneous frequency from the phase advance across the hop -
         * resolves to a small fraction of a bin (the vocoder's math). */
        float omega = TESSERA_TAU * (float)peak / (float)n;
        float dp    = tessera_wrap_pi(ph - t->phase[peak]
                                      - omega * (float)t->hop);
        float inst  = omega + dp / (float)t->hop;
        t->hz = inst * t->sr / TESSERA_TAU;
    } else {
        /* First frame after (re)priming: parabolic interpolation on the log
         * magnitudes around the peak gives the coarse sub-bin position. */
        float m2l = t->spec[peak - 1u].re * t->spec[peak - 1u].re +
                    t->spec[peak - 1u].im * t->spec[peak - 1u].im;
        float m2r = t->spec[peak + 1u].re * t->spec[peak + 1u].re +
                    t->spec[peak + 1u].im * t->spec[peak + 1u].im;
        float a = 0.5f * tessera_log2f(m2l > 0.0f ? m2l : 1e-30f);
        float b = 0.5f * tessera_log2f(pm2);
        float c = 0.5f * tessera_log2f(m2r > 0.0f ? m2r : 1e-30f);
        float den = a - 2.0f * b + c;
        float d = den != 0.0f ? 0.5f * (a - c) / den : 0.0f;
        d = tessera_clampf(d, -0.5f, 0.5f);
        t->hz = ((float)peak + d) * t->sr / (float)n;
    }

    for (uint32_t k = 1; k + 1u < bins; k++)
        t->phase[k] = tessera_atan2f(t->spec[k].im, t->spec[k].re);
    t->phase[peak] = ph;
    t->primed = 1;
}

void tessera_ftuner_process(tessera_ftuner_t *t, const float *x, uint32_t count)
{
    uint32_t n = t->n;
    for (uint32_t i = 0; i < count; i++) {
        t->ring[t->wpos] = x[i];
        t->wpos = (t->wpos + 1u) & (n - 1u);
        if (++t->fill >= t->hop) {
            t->fill = 0;
            ftuner_analyse(t);
        }
    }
}

float tessera_ftuner_hz(const tessera_ftuner_t *t) { return t->hz; }
