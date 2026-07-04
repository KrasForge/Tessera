/* sdk/lib/tessera_sampler.c - streaming sampler (Theme M15, issue #165).
 * See tessera.h.
 *
 * A bounded producer/consumer: the host pushes source samples into a fixed ring
 * off the audio path, and the audio path pulls resampled output.  Because the
 * ring is a fixed size, memory is bounded regardless of sample length - the
 * isolation guarantee a single-process host cannot make.  A refill that falls
 * behind yields silence, never a stall.
 */

#include "tessera.h"

#define SAMP_ONE  (1ull << 32)   /* Q32 unity */

void tessera_sampler_init(tessera_sampler_t *s, float *buf, uint32_t cap)
{
    s->buf    = buf;
    s->cap    = cap ? cap : 1u;
    s->filled = 0;
    s->play   = 0;
    s->pitch  = SAMP_ONE;
}

void tessera_sampler_set_pitch(tessera_sampler_t *s, float ratio)
{
    if (ratio < 0.0f) ratio = 0.0f;
    s->pitch = (uint64_t)(ratio * 4294967296.0f);   /* ratio * 2^32 */
}

void tessera_sampler_push(tessera_sampler_t *s, const float *src, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++)
        s->buf[(s->filled + k) % s->cap] = src[k];
    s->filled += n;
}

uint32_t tessera_sampler_headroom(const tessera_sampler_t *s)
{
    /* The window holds min(filled, cap) samples ending at `filled`; the host may
     * push up to cap ahead of the play cursor without overwriting unread data. */
    uint64_t play_i = s->play >> 32;
    uint64_t ahead  = s->filled > play_i ? s->filled - play_i : 0;
    return ahead >= s->cap ? 0u : (uint32_t)(s->cap - ahead);
}

float tessera_sampler_process(tessera_sampler_t *s)
{
    uint64_t i  = s->play >> 32;
    uint64_t lo = s->filled > s->cap ? s->filled - s->cap : 0;

    /* Fell behind the ring window (the host overwrote data we hadn't read):
     * skip forward to the oldest still-buffered sample rather than glitch. */
    if (i < lo) { s->play = lo << 32; i = lo; }

    /* Need i and i+1 buffered to interpolate; otherwise the stream has not been
     * fed far enough - underrun, hold position and emit silence. */
    if (i + 1 >= s->filled)
        return 0.0f;

    uint32_t frac = (uint32_t)(s->play & 0xffffffffu);
    float a = s->buf[i % s->cap];
    float b = s->buf[(i + 1) % s->cap];
    float out = a + (b - a) * ((float)frac * (1.0f / 4294967296.0f));

    s->play += s->pitch;
    return out;
}

uint64_t tessera_sampler_pos(const tessera_sampler_t *s)
{
    return s->play >> 32;
}
