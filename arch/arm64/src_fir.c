/* arch/arm64/src_fir.c - polyphase windowed-sinc sample-rate converter
 * (Theme M20, issue #192).  See src_fir.h. */

#include "src_fir.h"
#include "pcm_util.h"

#define ONE_Q32 (1ull << 32)

/* ---- init-time integer trig ------------------------------------------------
 * sin of a Q15 turn (p/32768 of a full circle) in Q15, via Bhaskara's
 * approximation (max error ~0.16% - well under the design window's sidelobe
 * floor).  Init-time only; the audio path never calls it. */
static int32_t isin_turn(uint32_t p)
{
    p &= 32767u;
    int neg = 0;
    if (p >= 16384u) { neg = 1; p -= 16384u; }
    int64_t t   = (int64_t)p * (int64_t)(16384u - p);      /* <= 2^26 */
    int64_t num = 16ll * t * 32768ll;
    int64_t den = (5ll << 28) - 4ll * t;
    int32_t s   = (int32_t)(num / den);
    return neg ? -s : s;
}

static int32_t icos_turn(uint32_t p) { return isin_turn(p + 8192u); }

/* sinc(x) = sin(pi x)/(pi x) in Q15, for x in Q15 (any magnitude). */
static int32_t isinc(int32_t x_q15)
{
    if (x_q15 < 0) x_q15 = -x_q15;
    if (x_q15 == 0)
        return 32768;
    /* sin(pi x): period 2 in x, so the Q15 turn phase is x/2 (mod a turn). */
    int32_t num = isin_turn((uint32_t)(x_q15 >> 1));
    /* pi * x in Q15 (pi = 102944/32768). */
    int64_t px = ((int64_t)x_q15 * 102944ll) >> 15;
    if (px == 0)
        return 32768;
    return (int32_t)(((int64_t)num * 32768ll) / px);
}

void src_fir_reset(src_fir_t *s)
{
    s->pos  = 0;
    s->n_in = 0;
    for (uint32_t i = 0; i < SRC_FIR_TAPS; i++)
        s->hist[i] = 0;
}

void src_fir_init(src_fir_t *s, uint32_t in_rate, uint32_t out_rate)
{
    if (in_rate == 0)  in_rate = 1;
    if (out_rate == 0) out_rate = 1;
    s->in_rate  = in_rate;
    s->out_rate = out_rate;
    s->step     = ((uint64_t)in_rate << 32) / out_rate;

    /* Anti-alias/anti-image cutoff: 0.92 of the narrower Nyquist, as a Q15
     * fraction of the input Nyquist (the 32-tap kernel's transition band
     * fits inside the remaining 8%). */
    uint32_t c_q15 = 30147u;                       /* 0.92 */
    if (out_rate < in_rate)
        c_q15 = (uint32_t)(((uint64_t)30147u * out_rate) / in_rate);
    if (c_q15 < 1024u) c_q15 = 1024u;              /* keep a usable band */

    const int half = (int)SRC_FIR_TAPS / 2;
    for (uint32_t q = 0; q <= SRC_FIR_PHASES; q++) {
        int32_t sum = 0;
        int     centre = 0;
        int32_t centre_mag = -1;
        for (uint32_t t = 0; t < SRC_FIR_TAPS; t++) {
            /* Tap distance from the output position, in input samples
             * (Q15): arg = q/PHASES + (TAPS/2 - 1 - t). */
            int32_t arg = (int32_t)((q << 15) / SRC_FIR_PHASES) +
                          (half - 1 - (int)t) * 32768;

            /* Windowed sinc: h = c * sinc(c*arg) * blackman(arg). */
            int32_t x  = (int32_t)(((int64_t)arg * (int64_t)c_q15) >> 15);
            int32_t sc = isinc(x);

            /* Blackman over the [-TAPS/2, TAPS/2] support:
             * w = 0.42 - 0.5 cos(2 pi u) + 0.08 cos(4 pi u), u in [0,1]. */
            int32_t u = (arg + (half << 15)) / (int32_t)SRC_FIR_TAPS;
            if (u < 0) u = 0;
            if (u > 32767) u = 32767;
            int32_t c2 = icos_turn((uint32_t)u);
            int32_t c4 = icos_turn((uint32_t)(2 * u));
            int32_t w  = 13763 - (int32_t)(((int64_t)16384 * c2) >> 15)
                               + (int32_t)(((int64_t)2621  * c4) >> 15);
            if (w < 0) w = 0;

            int64_t h = (((int64_t)sc * w) >> 15);
            h = (h * c_q15) >> 15;

            s->coef[q][t] = (int16_t)h;
            sum += (int32_t)h;
            int32_t mag = h < 0 ? (int32_t)-h : (int32_t)h;
            if (mag > centre_mag) { centre_mag = mag; centre = (int)t; }
        }
        /* Normalise: every phase sums to exactly 32768, so DC is bit-exact
         * (and any linear mix of two phases still sums to 32768). */
        s->coef[q][centre] = (int16_t)(s->coef[q][centre] + (32768 - sum));
    }

    src_fir_reset(s);
}

int src_fir_out_capacity(const src_fir_t *s, int n_in)
{
    if (n_in <= 0)
        return 0;
    uint64_t est = ((uint64_t)(uint32_t)n_in * s->out_rate) / s->in_rate + 2u;
    return (int)est;
}

int src_fir_process(src_fir_t *s, const int16_t *in, int n_in,
                    int16_t *out, int max_out)
{
    const uint32_t half = SRC_FIR_TAPS / 2u;
    int no = 0;

    for (int i = 0; i < n_in; i++) {
        /* Push into the history ring, indexed by absolute sample count. */
        s->hist[s->n_in & (SRC_FIR_TAPS - 1u)] = in[i];
        s->n_in++;

        /* Emit every output whose centred window is now fully available:
         * position p needs input up to p + TAPS/2. */
        while ((s->pos >> 32) + half + 1u <= s->n_in) {
            uint64_t ip   = s->pos >> 32;
            uint32_t frac = (uint32_t)s->pos;
            uint32_t q    = frac >> 27;                    /* top 5 bits   */
            int32_t  r    = (int32_t)((frac >> 12) & 0x7fffu);

            const int16_t *ca = s->coef[q];
            const int16_t *cb = s->coef[q + 1u];
            uint64_t base = ip - half + 1u;                /* oldest tap   */

            /* Exact Q30 accumulation: the per-tap coefficient is
             * ca*(32768-r) + cb*r, kept unrounded so the coefficient SUM of
             * any phase mix stays exactly 32768<<15 - which is what makes DC
             * bit-exact through the resampler. */
            int32_t ra = 32768 - r;
            int64_t acc = 0;
            for (uint32_t t = 0; t < SRC_FIR_TAPS; t++) {
                int64_t c = (int64_t)ca[t] * ra + (int64_t)cb[t] * r;  /* Q30 */
                acc += (int64_t)s->hist[(base + t) & (SRC_FIR_TAPS - 1u)] * c;
            }
            int32_t y = (int32_t)((acc + (1ll << 29)) >> 30);
            if (no < max_out)
                out[no] = sat16(y);
            no++;
            s->pos += s->step;
        }
    }
    return no;
}
