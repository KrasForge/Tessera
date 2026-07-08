/* sdk/lib/tessera_pvoc.c - phase vocoder: time-stretch and pitch-shift for
 * the Tessera SDK (Theme M18, issue #186).
 *
 * An STFT analysis/synthesis framework over the FFT primitive (#184): Hann
 * analysis and synthesis windows at 75% overlap, forward rFFT, per-bin phase
 * propagation, inverse rFFT, weighted overlap-add.  The synthesis hop is
 * fixed at n/4 - exactly the hop at which the squared Hann window sums to the
 * constant 3/2, so resynthesis is COLA-exact - and the ANALYSIS hop is derived
 * from the requested ratio (ha = round(hs / ratio)), so the effective ratio is
 * the exact rational hs/ha and never drifts.
 *
 * Phase coherence: spectral peaks propagate by their instantaneous frequency
 * (the analysis phase increment heterodyned against the bin's centre
 * frequency); every other bin is IDENTITY-PHASE-LOCKED to its region's peak,
 * preserving the analysis phase offsets, so each synthesis grain is the
 * analysis grain under one coherent rotation.  Independent per-bin
 * propagation would let a tone's mainlobe bins drift apart at ratio != 1 and
 * the overlapped grains partially cancel - the classic vocoder smearing; the
 * locking removes it.  At unity ratio the propagated phase equals the
 * analysis phase modulo 2*pi, so unity settings reproduce the input (minus
 * the n-sample framework latency).
 *
 * tessera_pshift_* wraps the vocoder into a same-length pitch shifter:
 * time-stretch by the pitch ratio, then linear-resample the stretched stream
 * back to the original duration, which multiplies every frequency by the
 * ratio.  Streaming, block-size agnostic, and real-time safe: the caller owns
 * every buffer, nothing allocates, per-call work is bounded.  No libc; no
 * libm - sqrt and atan2 are self-contained (bit-trick seed + Newton, odd
 * polynomial with octant fixup), and resynthesis trig is tessera_sinf.
 */

#include "tessera.h"

/* ---- self-contained float helpers (no libm) ------------------------------ */

static float pv_sqrtf(float x)
{
    if (x <= 0.0f)
        return 0.0f;
    union { float f; uint32_t u; } v = { .f = x };
    v.u = (v.u >> 1) + 0x1fbd1df5u;               /* seed: ~sqrt via exponent */
    float y = v.f;
    y = 0.5f * (y + x / y);                        /* two Newton steps        */
    y = 0.5f * (y + x / y);
    return y;
}

/* atan on [0,1] - minimax-ish odd polynomial, |err| < 1e-4 rad. */
static float pv_atan01(float z)
{
    float z2 = z * z;
    return z * (0.99997726f +
           z2 * (-0.33262347f +
           z2 * ( 0.19354346f +
           z2 * (-0.11643287f +
           z2 * ( 0.05265332f +
           z2 * (-0.01172120f))))));
}

#define PV_PI  3.14159265358979323846f
#define PV_TAU 6.28318530717958647692f

static float pv_atan2f(float y, float x)
{
    float ay = y < 0.0f ? -y : y;
    float ax = x < 0.0f ? -x : x;
    if (ax == 0.0f && ay == 0.0f)
        return 0.0f;
    /* atan of the smaller/larger ratio, then octant fixup. */
    float a = ax >= ay ? pv_atan01(ay / ax) : PV_PI / 2.0f - pv_atan01(ax / ay);
    if (x < 0.0f) a = PV_PI - a;
    return y < 0.0f ? -a : a;
}

static float pv_cosf(float x) { return tessera_sinf(x + PV_PI / 2.0f); }

/* Wrap an angle to (-pi, pi]. */
static float pv_wrap(float p)
{
    float k = p * (1.0f / PV_TAU);
    int   ki = (int)(k + (k >= 0.0f ? 0.5f : -0.5f));
    return p - (float)ki * PV_TAU;
}

/* ---- STFT vocoder core ---------------------------------------------------- */

uint32_t tessera_pvoc_floats(uint32_t n) { return 4u * n + 4u * (n / 2u + 1u); }
uint32_t tessera_pvoc_cpx(uint32_t n)    { return n / 2u + 1u; }

int tessera_pvoc_init(tessera_pvoc_t *pv, uint32_t n, float ratio,
                      const tessera_cpx_t *tw, float *mem, tessera_cpx_t *cmem)
{
    if (!pv || !tw || !mem || !cmem)
        return -1;
    if (n < 64u || (n & (n - 1u)) != 0u)
        return -1;
    if (!(ratio > 0.24f) || !(ratio < 4.01f))
        return -1;

    pv->n  = n;
    pv->hs = n / 4u;
    /* ha from the ratio; the effective ratio is exactly hs/ha. */
    uint32_t ha = (uint32_t)((float)pv->hs / ratio + 0.5f);
    if (ha == 0u) ha = 1u;
    if (ha > n)   ha = n;
    pv->ha = ha;
    pv->tw = tw;

    uint32_t bins = n / 2u + 1u;
    pv->win         = mem;                mem += n;
    pv->in_ring     = mem;                mem += n;
    pv->ola         = mem;                mem += n;
    pv->frame       = mem;                mem += n;
    pv->prev_phase  = mem;                mem += bins;
    pv->synth_phase = mem;                mem += bins;
    pv->aphase      = mem;                mem += bins;
    pv->peaks       = mem;
    pv->spec        = cmem;

    tessera_window_hann(pv->win, n);
    tessera_pvoc_reset(pv);
    return 0;
}

void tessera_pvoc_reset(tessera_pvoc_t *pv)
{
    uint32_t n = pv->n, bins = n / 2u + 1u;
    for (uint32_t i = 0; i < n; i++) {
        pv->in_ring[i] = 0.0f;
        pv->ola[i]     = 0.0f;
    }
    for (uint32_t k = 0; k < bins; k++) {
        pv->prev_phase[k]  = 0.0f;
        pv->synth_phase[k] = 0.0f;
    }
    pv->primed = 0;
}

void tessera_pvoc_process(tessera_pvoc_t *pv, const float *in, float *out)
{
    uint32_t n = pv->n, ha = pv->ha, hs = pv->hs, bins = n / 2u + 1u;

    /* Slide `ha` new samples into the analysis ring (last n input samples). */
    for (uint32_t i = 0; i < n - ha; i++)
        pv->in_ring[i] = pv->in_ring[i + ha];
    for (uint32_t i = 0; i < ha; i++)
        pv->in_ring[n - ha + i] = in[i];

    /* Analysis: window + forward transform. */
    for (uint32_t i = 0; i < n; i++)
        pv->frame[i] = pv->in_ring[i] * pv->win[i];
    tessera_rfft(pv->frame, pv->spec, pv->tw, n);

    /* Polar form: magnitudes stay in spec[].re, analysis phases in aphase. */
    for (uint32_t k = 0; k < bins; k++) {
        float re = pv->spec[k].re, im = pv->spec[k].im;
        pv->spec[k].re = pv_sqrtf(re * re + im * im);
        pv->aphase[k]  = pv_atan2f(im, re);
    }

    /* Phase propagation with identity phase locking (vertical coherence).
     * Propagating every bin independently lets a tone's mainlobe bins drift
     * against each other at ratio != 1 - the overlapped grains partially
     * cancel (audible smearing and level loss).  Instead, only spectral
     * PEAKS propagate by their instantaneous frequency; every other bin is
     * locked to its region's peak with the analysis phase offset preserved,
     * so each synthesis grain is the analysis grain under one coherent
     * rotation.  At unity ratio this reduces to psi == phase exactly. */
    uint32_t np = 0;
    if (pv->primed) {
        for (uint32_t k = 0; k < bins; k++) {
            float m  = pv->spec[k].re;
            float ml = k > 0u        ? pv->spec[k - 1u].re : 0.0f;
            float mr = k + 1u < bins ? pv->spec[k + 1u].re : 0.0f;
            if (m > ml && m >= mr && m > 1e-9f)
                pv->peaks[np++] = (float)k;
        }
    }

    if (!pv->primed || np == 0u) {
        /* First frame (or silence): lock synthesis to the analysis phases. */
        for (uint32_t k = 0; k < bins; k++)
            pv->synth_phase[k] = pv->aphase[k];
    } else {
        /* Propagate each peak by its instantaneous frequency. */
        for (uint32_t p = 0; p < np; p++) {
            uint32_t k = (uint32_t)pv->peaks[p];
            float omega = PV_TAU * (float)k / (float)n;
            float dp = pv_wrap(pv->aphase[k] - pv->prev_phase[k]
                               - omega * (float)ha);
            float inst = omega + dp / (float)ha;
            pv->synth_phase[k] =
                pv_wrap(pv->synth_phase[k] + inst * (float)hs);
        }
        /* Lock every other bin to its nearest peak (regions split halfway
         * between neighbouring peaks). */
        for (uint32_t p = 0; p < np; p++) {
            uint32_t k     = (uint32_t)pv->peaks[p];
            uint32_t lo    = p == 0u ? 0u
                           : (k + (uint32_t)pv->peaks[p - 1u] + 1u) / 2u;
            uint32_t hi    = p + 1u < np
                           ? ((uint32_t)pv->peaks[p + 1u] + k) / 2u
                           : bins - 1u;
            float    psi_p = pv->synth_phase[k];
            float    phi_p = pv->aphase[k];
            for (uint32_t b = lo; b <= hi; b++) {
                if (b == k) continue;
                pv->synth_phase[b] = pv_wrap(psi_p + pv->aphase[b] - phi_p);
            }
        }
    }

    for (uint32_t k = 0; k < bins; k++) {
        float mag = pv->spec[k].re;
        float psi = pv->synth_phase[k];
        pv->prev_phase[k] = pv->aphase[k];
        pv->spec[k].re = mag * pv_cosf(psi);
        pv->spec[k].im = mag * tessera_sinf(psi);
    }
    pv->primed = 1;

    /* Synthesis: inverse transform, synthesis window, weighted overlap-add
     * (Hann^2 at 75% overlap sums to 3/2). */
    tessera_irfft(pv->spec, pv->frame, pv->tw, n);
    for (uint32_t i = 0; i < n; i++)
        pv->ola[i] += pv->frame[i] * pv->win[i] * (1.0f / 1.5f);

    for (uint32_t i = 0; i < hs; i++)
        out[i] = pv->ola[i];
    for (uint32_t i = 0; i < n - hs; i++)
        pv->ola[i] = pv->ola[i + hs];
    for (uint32_t i = n - hs; i < n; i++)
        pv->ola[i] = 0.0f;
}

/* ---- pitch shifter (stretch + resample back) ------------------------------ */

uint32_t tessera_pshift_floats(uint32_t n)
{
    return tessera_pvoc_floats(n) + 6u * n;
}

int tessera_pshift_init(tessera_pshift_t *ps, uint32_t n, float ratio,
                        const tessera_cpx_t *tw, float *mem, tessera_cpx_t *cmem)
{
    if (!ps || !mem)
        return -1;
    /* The vocoder stretches time by `ratio`; reading the stretched stream at
     * step `ratio` restores the duration and scales pitch by `ratio`. */
    if (tessera_pvoc_init(&ps->pv, n, ratio, tw,
                          mem + 6u * n, cmem) != 0)
        return -1;
    ps->ratio    = (float)ps->pv.hs / (float)ps->pv.ha;   /* exact rational  */
    ps->in_fifo  = mem;                    /* 2n */
    ps->st_fifo  = mem + 2u * n;           /* 2n */
    ps->out_fifo = mem + 4u * n;           /* 2n */
    ps->cap      = 2u * n;
    tessera_pshift_reset(ps);
    return 0;
}

void tessera_pshift_reset(tessera_pshift_t *ps)
{
    tessera_pvoc_reset(&ps->pv);
    ps->in_n = ps->st_n = ps->out_n = 0;
    ps->rpos = 0.0f;
}

void tessera_pshift_process(tessera_pshift_t *ps, const float *in, float *out,
                            uint32_t count)
{
    uint32_t ha = ps->pv.ha, hs = ps->pv.hs;
    uint32_t fed = 0, done = 0;

    for (;;) {
        /* Feed whatever input fits into the FIFO. */
        uint32_t chunk = count - fed;
        if (chunk > ps->cap - ps->in_n)
            chunk = ps->cap - ps->in_n;
        for (uint32_t i = 0; i < chunk; i++)
            ps->in_fifo[ps->in_n + i] = in[fed + i];
        ps->in_n += chunk;
        fed += chunk;

        /* Run whole hops through the vocoder: consume ha, produce hs. */
        while (ps->in_n >= ha && ps->st_n + hs <= ps->cap) {
            tessera_pvoc_process(&ps->pv, ps->in_fifo, ps->st_fifo + ps->st_n);
            ps->in_n -= ha;
            for (uint32_t i = 0; i < ps->in_n; i++)
                ps->in_fifo[i] = ps->in_fifo[i + ha];
            ps->st_n += hs;
        }

        /* Resample the stretched stream at step `ratio` (linear interp). */
        while (ps->out_n < ps->cap && ps->rpos + 1.0f < (float)ps->st_n) {
            uint32_t i0 = (uint32_t)ps->rpos;
            float    fr = ps->rpos - (float)i0;
            ps->out_fifo[ps->out_n++] =
                ps->st_fifo[i0] + (ps->st_fifo[i0 + 1u] - ps->st_fifo[i0]) * fr;
            ps->rpos += ps->ratio;
        }
        /* Drop fully-consumed stretched samples. */
        uint32_t drop = (uint32_t)ps->rpos;
        if (drop > 0u) {
            if (drop > ps->st_n) drop = ps->st_n;
            for (uint32_t i = 0; i < ps->st_n - drop; i++)
                ps->st_fifo[i] = ps->st_fifo[i + drop];
            ps->st_n -= drop;
            ps->rpos -= (float)drop;
        }

        /* Emit what is ready. */
        uint32_t want = count - done;
        uint32_t take = ps->out_n < want ? ps->out_n : want;
        for (uint32_t i = 0; i < take; i++)
            out[done + i] = ps->out_fifo[i];
        for (uint32_t i = 0; i < ps->out_n - take; i++)
            ps->out_fifo[i] = ps->out_fifo[i + take];
        ps->out_n -= take;
        done += take;

        if (done >= count)
            return;
        /* No more input to feed and not enough buffered for another hop:
         * the remainder is start-up priming - pad with silence. */
        if (fed >= count && ps->in_n < ha) {
            for (uint32_t i = done; i < count; i++)
                out[i] = 0.0f;
            return;
        }
    }
}
