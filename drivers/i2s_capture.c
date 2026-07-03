/* drivers/i2s_capture.c - I2S capture ring buffer (Issue #83, M14) */

#include "i2s_capture.h"

void i2s_capture_init(i2s_capture_t *c, int16_t *storage,
                      uint32_t n_blocks, uint32_t frames)
{
    c->storage   = storage;
    c->frames    = frames;
    c->n_blocks  = n_blocks;
    c->head      = 0;
    c->tail      = 0;
    c->count     = 0;
    c->produced  = 0;
    c->consumed  = 0;
    c->overruns  = 0;
    c->underruns = 0;
}

static int16_t *slot(i2s_capture_t *c, uint32_t index)
{
    return c->storage + (uint64_t)index * c->frames * 2u;
}

void i2s_capture_produce(i2s_capture_t *c, const int16_t *block)
{
    if (c->n_blocks == 0)
        return;

    /* Full: drop the oldest unread block so the newest always lands.  The
     * hardware producer cannot be stalled, so this is the only safe policy. */
    if (c->count == c->n_blocks) {
        c->tail = (c->tail + 1u) % c->n_blocks;
        c->count--;
        c->overruns++;
    }

    int16_t *dst = slot(c, c->head);
    for (uint32_t i = 0; i < c->frames * 2u; i++)
        dst[i] = block[i];
    c->head = (c->head + 1u) % c->n_blocks;
    c->count++;
    c->produced++;
}

int i2s_capture_consume(i2s_capture_t *c, int16_t *out)
{
    if (c->count == 0) {
        for (uint32_t i = 0; i < c->frames * 2u; i++)
            out[i] = 0;                      /* silence on underrun */
        c->underruns++;
        return 0;
    }

    const int16_t *src = slot(c, c->tail);
    for (uint32_t i = 0; i < c->frames * 2u; i++)
        out[i] = src[i];
    c->tail = (c->tail + 1u) % c->n_blocks;
    c->count--;
    c->consumed++;
    return 1;
}

uint32_t i2s_capture_available(const i2s_capture_t *c)
{
    return c->count;
}
