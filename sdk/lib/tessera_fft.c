/* sdk/lib/tessera_fft.c - FFT primitive for the Tessera SDK (Theme M18,
 * issue #184).
 *
 * The single most enabling DSP primitive: one FFT unlocks partitioned
 * convolution, spectral effects (phase vocoder, spectral gate), and real
 * analysis (spectrum display, a robust tuner).  A radix-2 decimation-in-time
 * complex FFT and inverse, in place, for power-of-two sizes, plus the packed
 * real-input pair (rFFT/irFFT) since audio is real, and the Hann/Hamming
 * window helpers spectral processing needs.
 *
 * Everything is allocation-free: the caller owns the twiddle table (filled
 * once by tessera_fft_twiddles) and the data buffers.  No libc, no libm - the
 * twiddles are generated at setup time by a self-contained double-precision
 * Taylor sine (setup runs off the audio path, so the extra precision is free);
 * the per-block transform is single-precision float and does bounded work.
 */

#include "tessera.h"

/* ---- setup-time trig (double, Taylor series after range reduction) --------
 * Twiddle accuracy bounds the whole transform's noise floor, so setup uses a
 * ~1e-15 sine rather than the fast audio-path tessera_sinf (~1e-3).  Setup
 * only: never called from process paths. */

static double tw_sin(double x)
{
    const double tau = 6.283185307179586476925286766559;
    /* Range-reduce to [-pi, pi].  |x| stays tiny here (one period at most a
     * few thousand twiddles), so the naive reduction loses nothing. */
    double k = x / tau;
    double ki = (double)(long)(k + (k >= 0.0 ? 0.5 : -0.5));
    x -= ki * tau;

    /* Taylor about 0 on [-pi, pi]; 11 terms keep the tail < 1e-15. */
    double x2 = x * x, term = x, sum = x;
    for (int i = 1; i <= 10; i++) {
        term *= -x2 / (double)((2 * i) * (2 * i + 1));
        sum  += term;
    }
    return sum;
}

static double tw_cos(double x)
{
    return tw_sin(x + 1.5707963267948966192313216916398);
}

static int is_pow2(uint32_t n) { return n != 0u && (n & (n - 1u)) == 0u; }

void tessera_fft_twiddles(tessera_cpx_t *tw, uint32_t n)
{
    if (!tw || !is_pow2(n) || n < 2u)
        return;
    const double tau = 6.283185307179586476925286766559;
    for (uint32_t k = 0; k < n / 2u; k++) {
        double a = -tau * (double)k / (double)n;      /* e^{-i 2 pi k / n} */
        tw[k].re = (float)tw_cos(a);
        tw[k].im = (float)tw_sin(a);
    }
}

/* ---- radix-2 DIT core -----------------------------------------------------
 * `tw_stride` lets the packed real transform reuse a twiddle table generated
 * for 2n: the n-point FFT reads every tw_stride-th entry. */

static void fft_core(tessera_cpx_t *x, const tessera_cpx_t *tw,
                     uint32_t n, uint32_t tw_stride)
{
    /* Bit-reversal permutation. */
    for (uint32_t i = 1u, j = 0u; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j |= bit;
        if (i < j) {
            tessera_cpx_t t = x[i]; x[i] = x[j]; x[j] = t;
        }
    }

    /* Butterflies, stage lengths 2, 4, ..., n. */
    for (uint32_t len = 2u; len <= n; len <<= 1) {
        uint32_t half = len >> 1;
        uint32_t step = (n / len) * tw_stride;
        for (uint32_t base = 0; base < n; base += len) {
            for (uint32_t k = 0; k < half; k++) {
                tessera_cpx_t w = tw[k * step];
                tessera_cpx_t *a = &x[base + k];
                tessera_cpx_t *b = &x[base + k + half];
                float tre = b->re * w.re - b->im * w.im;
                float tim = b->re * w.im + b->im * w.re;
                b->re = a->re - tre;
                b->im = a->im - tim;
                a->re += tre;
                a->im += tim;
            }
        }
    }
}

void tessera_fft(tessera_cpx_t *x, const tessera_cpx_t *tw, uint32_t n)
{
    if (!x || !tw || !is_pow2(n) || n < 2u)
        return;
    fft_core(x, tw, n, 1u);
}

void tessera_ifft(tessera_cpx_t *x, const tessera_cpx_t *tw, uint32_t n)
{
    if (!x || !tw || !is_pow2(n) || n < 2u)
        return;
    /* IFFT(x) = conj(FFT(conj(x))) / n */
    for (uint32_t i = 0; i < n; i++)
        x[i].im = -x[i].im;
    fft_core(x, tw, n, 1u);
    float s = 1.0f / (float)n;
    for (uint32_t i = 0; i < n; i++) {
        x[i].re =  x[i].re * s;
        x[i].im = -x[i].im * s;
    }
}

/* ---- packed real transforms ----------------------------------------------
 * n real samples cost one n/2-point complex FFT plus an O(n) split: pack
 * z[j] = in[2j] + i*in[2j+1], transform, then separate the interleaved even/
 * odd spectra.  Twiddles are the caller's n-table (tessera_fft_twiddles(tw,n));
 * the half-size FFT strides through it. */

void tessera_rfft(const float *in, tessera_cpx_t *out,
                  const tessera_cpx_t *tw, uint32_t n)
{
    if (!in || !out || !tw || !is_pow2(n) || n < 4u)
        return;
    uint32_t h = n / 2u;

    for (uint32_t j = 0; j < h; j++) {
        out[j].re = in[2u * j];
        out[j].im = in[2u * j + 1u];
    }
    fft_core(out, tw, h, 2u);

    /* Split.  With E/O the even/odd-sample spectra:
     *   X[k] = E[k] + e^{-i 2 pi k / n} O[k]
     * where E[k] = (Z[k] + conj(Z[h-k]))/2, O[k] = -i(Z[k] - conj(Z[h-k]))/2.
     * Process k and h-k as a pair so it runs in place; k = 0 yields both DC
     * and Nyquist (out[h]). */
    float z0re = out[0].re, z0im = out[0].im;
    out[0].re = z0re + z0im;  out[0].im = 0.0f;       /* DC      */
    out[h].re = z0re - z0im;  out[h].im = 0.0f;       /* Nyquist */

    for (uint32_t k = 1; k <= h / 2u; k++) {
        uint32_t m = h - k;
        tessera_cpx_t a = out[k], b = out[m];

        float ere =  0.5f * (a.re + b.re);
        float eim =  0.5f * (a.im - b.im);
        float ore =  0.5f * (a.im + b.im);            /* -i(a-conj(b))/2 */
        float oim = -0.5f * (a.re - b.re);

        tessera_cpx_t w = tw[k];                      /* e^{-i 2 pi k / n} */
        float wore = ore * w.re - oim * w.im;
        float woim = ore * w.im + oim * w.re;

        out[k].re = ere + wore;
        out[k].im = eim + woim;
        if (m != k) {
            /* The mirror bin comes from the same pair: E[m] = conj(E[k]),
             * O[m] = conj(O[k]), and the twiddle at m is -conj(w), so
             * X[m] = conj(E[k]) - conj(w O[k]). */
            out[m].re =  ere - wore;
            out[m].im = -eim + woim;
        }
    }
}

void tessera_irfft(tessera_cpx_t *in, float *out,
                   const tessera_cpx_t *tw, uint32_t n)
{
    if (!in || !out || !tw || !is_pow2(n) || n < 4u)
        return;
    uint32_t h = n / 2u;

    /* Repack bins X[0..h] into the half-size spectrum Z[k] (inverse of the
     * split above), reusing `in` as the workspace. */
    float dc = in[0].re, ny = in[h].re;
    in[0].re = 0.5f * (dc + ny);
    in[0].im = 0.5f * (dc - ny);

    for (uint32_t k = 1; k <= h / 2u; k++) {
        uint32_t m = h - k;
        tessera_cpx_t xk = in[k], xm = in[m];

        float ere = 0.5f * (xk.re + xm.re);
        float eim = 0.5f * (xk.im - xm.im);
        float wore = 0.5f * (xk.re - xm.re);
        float woim = 0.5f * (xk.im + xm.im);

        /* Undo the twiddle: O = conj(w) * (wO). */
        tessera_cpx_t w = tw[k];
        float ore = wore * w.re + woim * w.im;
        float oim = woim * w.re - wore * w.im;

        /* Z[k] = E + iO, Z[m] = conj(E) + i conj(O). */
        in[k].re = ere - oim;
        in[k].im = eim + ore;
        if (m != k) {
            in[m].re =  ere + oim;
            in[m].im = -eim + ore;
        }
    }

    /* Half-size inverse FFT (conjugate trick, stride-2 twiddles). */
    for (uint32_t i = 0; i < h; i++)
        in[i].im = -in[i].im;
    fft_core(in, tw, h, 2u);
    float s = 1.0f / (float)h;
    for (uint32_t j = 0; j < h; j++) {
        out[2u * j]      =  in[j].re * s;
        out[2u * j + 1u] = -in[j].im * s;
    }
}

/* ---- windows (periodic, for STFT overlap-add) ----------------------------- */

void tessera_window_hann(float *w, uint32_t n)
{
    if (!w || n == 0u)
        return;
    const double tau = 6.283185307179586476925286766559;
    for (uint32_t i = 0; i < n; i++)
        w[i] = (float)(0.5 - 0.5 * tw_cos(tau * (double)i / (double)n));
}

void tessera_window_hamming(float *w, uint32_t n)
{
    if (!w || n == 0u)
        return;
    const double tau = 6.283185307179586476925286766559;
    for (uint32_t i = 0; i < n; i++)
        w[i] = (float)(0.54 - 0.46 * tw_cos(tau * (double)i / (double)n));
}
