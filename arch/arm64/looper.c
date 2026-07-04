/* arch/arm64/looper.c - multi-track loop pedal (Theme M17, issue #172).
 * See looper.h. */

#include "looper.h"
#include "pcm_util.h"

void looper_init(looper_t *l, int16_t *const *tracks, int n_tracks,
                 uint32_t cap, uint32_t quantum)
{
    if (n_tracks > LOOPER_MAX_TRACKS) n_tracks = LOOPER_MAX_TRACKS;
    l->n_tracks = n_tracks;
    l->cap      = cap ? cap : 1u;
    l->quantum  = quantum ? quantum : 1u;
    for (int i = 0; i < n_tracks; i++) l->track[i] = tracks[i];
    looper_clear(l);
}

void looper_clear(looper_t *l)
{
    l->state    = LOOP_IDLE;
    l->loop_len = 0;
    l->pos      = 0;
    l->layers   = 0;
    l->cur      = 0;
    for (int t = 0; t < l->n_tracks; t++)
        for (uint32_t i = 0; i < l->cap; i++)
            l->track[t][i] = 0;
}

loop_state_t looper_record(looper_t *l)
{
    if (l->state == LOOP_IDLE) {
        /* First layer: record into track 0, growing the loop until stop. */
        l->state = LOOP_RECORDING;
        l->cur   = 0;
        l->pos   = 0;
    } else if (l->state == LOOP_PLAYING) {
        /* Overdub onto the next free track, in sync with the loop. */
        if (l->layers < l->n_tracks) {
            l->state = LOOP_OVERDUB;
            l->cur   = l->layers;
            /* pos keeps running with the loop so the overdub stays aligned. */
        }
    }
    return l->state;
}

/* Ramp the first and last LOOPER_DECLICK samples of a track's active region so
 * the loop seam and punch edges are click-free. */
static void declick(int16_t *buf, uint32_t len)
{
    uint32_t r = LOOPER_DECLICK;
    if (r > len / 2) r = len / 2;
    for (uint32_t i = 0; i < r; i++) {
        int32_t g = (int32_t)((i * 32768u) / r);          /* 0 -> ~1.0 Q15 */
        buf[i]           = sat16(((int32_t)buf[i] * g) >> 15);
        buf[len - 1 - i] = sat16(((int32_t)buf[len - 1 - i] * g) >> 15);
    }
}

loop_state_t looper_stop(looper_t *l)
{
    if (l->state == LOOP_RECORDING) {
        /* Snap the loop length to the nearest quantise-grid multiple (>= 1). */
        uint32_t q = l->quantum;
        uint32_t len = ((l->pos + q / 2) / q) * q;
        if (len == 0) len = q;
        if (len > l->cap) len = (l->cap / q) * q;
        if (len == 0) len = l->cap;
        l->loop_len = len;
        l->layers   = 1;
        declick(l->track[0], l->loop_len);
        l->state = LOOP_PLAYING;
        l->pos   = 0;
    } else if (l->state == LOOP_OVERDUB) {
        declick(l->track[l->cur], l->loop_len);
        if (l->cur + 1 > l->layers) l->layers = l->cur + 1;
        l->state = LOOP_PLAYING;
    }
    return l->state;
}

int16_t looper_process(looper_t *l, int16_t in)
{
    switch (l->state) {
    case LOOP_IDLE:
        return in;                                   /* passthrough */

    case LOOP_RECORDING:
        if (l->pos < l->cap) {
            l->track[0][l->pos] = in;
            l->pos++;
        }
        if (l->pos >= l->cap)                         /* hit the memory bound */
            looper_stop(l);
        return in;                                    /* monitor the input */

    case LOOP_PLAYING:
    case LOOP_OVERDUB: {
        if (l->state == LOOP_OVERDUB) {
            /* Sum the input into the current overdub track. */
            int32_t v = (int32_t)l->track[l->cur][l->pos] + in;
            l->track[l->cur][l->pos] = sat16(v);
        }
        int32_t mix = 0;
        for (int t = 0; t < l->layers; t++)
            mix += l->track[t][l->pos];
        int16_t out = sat16(mix);
        l->pos = (l->pos + 1u) % l->loop_len;
        return out;
    }
    }
    return in;
}
