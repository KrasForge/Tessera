/* arch/arm64/limiter.c - master output limiter / soft-clip (Theme M15, #166).
 * See limiter.h. */

#include "limiter.h"
#include "pcm_util.h"

#define LIM_ONE 32768   /* Q15 unity gain */

static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

void limiter_init(limiter_t *l, int16_t *dbuf, int32_t *gbuf, int lookahead,
                  int32_t ceiling, int32_t release_step_q15)
{
    if (lookahead < 1) lookahead = 1;
    if (ceiling < 1) ceiling = 1;
    if (ceiling > 32767) ceiling = 32767;
    if (release_step_q15 < 1) release_step_q15 = 1;

    l->dbuf      = dbuf;
    l->gbuf      = gbuf;
    l->lookahead = lookahead;
    l->w         = 0;
    l->ceiling   = ceiling;
    l->gain      = LIM_ONE;
    l->rel_step  = release_step_q15;
    for (int i = 0; i < lookahead; i++) { dbuf[i] = 0; gbuf[i] = LIM_ONE; }
}

int16_t limiter_process(limiter_t *l, int16_t x)
{
    /* Gain that would just contain this sample at the ceiling. */
    int32_t a = iabs((int32_t)x);
    int32_t inst = (a <= l->ceiling || a == 0)
                 ? LIM_ONE
                 : (l->ceiling * LIM_ONE) / a;      /* < LIM_ONE */

    /* The sample leaving the look-ahead window (oldest) is the one we output. */
    int16_t x_out = l->dbuf[l->w];

    /* Windowed minimum gain over the whole window - the samples currently in the
     * ring (including x_out at slot w) AND the just-arrived sample.  Computed
     * BEFORE overwriting x_out's slot, so its own constraint is included: that is
     * what guarantees x_out is scaled to within the ceiling as it leaves, while
     * an incoming peak also pulls the gain down ahead of time. */
    int32_t win = inst;
    for (int i = 0; i < l->lookahead; i++)
        if (l->gbuf[i] < win) win = l->gbuf[i];

    l->dbuf[l->w] = x;
    l->gbuf[l->w] = inst;
    l->w = (l->w + 1) % l->lookahead;

    /* Recover toward `win` at the release rate, but never rise above it (so the
     * brick-wall guarantee holds); a lower `win` (a new peak) drops us instantly. */
    int32_t g = l->gain + l->rel_step;
    if (g > win) g = win;
    l->gain = g;

    return sat16(((int32_t)x_out * g) >> 15);
}

void limiter_block(limiter_t *l, int16_t *buf, int n)
{
    for (int i = 0; i < n; i++)
        buf[i] = limiter_process(l, buf[i]);
}

int limiter_latency(const limiter_t *l) { return l->lookahead; }

int16_t limiter_softclip(int16_t x, int32_t ceiling)
{
    if (ceiling < 1) ceiling = 1;
    if (ceiling > 32767) ceiling = 32767;

    int32_t s = x < 0 ? -1 : 1;
    int32_t t = iabs((int32_t)x);

    /* Soft knee at half the ceiling: below it the signal is transparent
     * (unit gain); above it a rational curve rounds smoothly to the ceiling and
     * never exceeds it.  y = knee + den*num/(den+num), where den = ceiling-knee
     * and num = t-knee, which has unit slope at the knee and asymptote `ceiling`. */
    int32_t knee = (ceiling * 4) / 5;      /* transparent below 0.8 * ceiling */
    if (t <= knee)
        return x;                                       /* transparent, bit-exact */

    int32_t den = ceiling - knee;
    int32_t num = t - knee;
    int32_t y   = knee + (int32_t)(((int64_t)den * num) / (den + num));
    if (y > ceiling) y = ceiling;
    return (int16_t)(s * y);
}
