/* arch/arm64/src.c - sample-rate conversion (Theme H, issue #131).  See src.h. */

#include "src.h"
#include "pcm_util.h"

#define SRC_ONE  (1ull << 32)   /* Q32 unity */

void src_init(src_t *s, uint32_t in_rate, uint32_t out_rate)
{
    if (in_rate == 0)  in_rate = 1;
    if (out_rate == 0) out_rate = 1;
    s->in_rate  = in_rate;
    s->out_rate = out_rate;
    /* Input samples to advance per output sample, Q32: in_rate / out_rate. */
    s->step = ((uint64_t)in_rate << 32) / out_rate;
    src_reset(s);
}

void src_reset(src_t *s)
{
    s->frac   = 0;
    s->prev   = 0;
    s->primed = 0;
}

int src_out_capacity(const src_t *s, int n_in)
{
    if (n_in <= 0)
        return 0;
    /* Each input spans one window; outputs per window ~= out_rate/in_rate.  Add a
     * couple of samples of slack for the partial window carried in `frac`. */
    uint64_t est = ((uint64_t)n_in * s->out_rate) / s->in_rate + 2u;
    return (int)est;
}

int src_process(src_t *s, const int16_t *in, int n_in, int16_t *out, int max_out)
{
    int no = 0;
    int i  = 0;

    /* Prime with the first input sample so interpolation always has a valid
     * left endpoint (`prev`); the very first window is [in[0], in[1]). */
    if (!s->primed) {
        if (n_in <= 0)
            return 0;
        s->prev   = in[0];
        s->frac   = 0;
        s->primed = 1;
        i = 1;
    }

    for (; i < n_in; i++) {
        int32_t cur = in[i];
        /* Emit every output whose position falls inside [prev, cur). */
        while (s->frac < SRC_ONE) {
            int64_t d = (int64_t)cur - (int64_t)s->prev;
            int32_t interp = s->prev + (int32_t)((d * (int64_t)s->frac) >> 32);
            if (no < max_out)
                out[no] = sat16(interp);
            no++;
            s->frac += s->step;
        }
        s->frac -= SRC_ONE;      /* advance the window by exactly one input */
        s->prev  = cur;
    }
    return no;
}
