/* drivers/audio.c - DMA double-buffered audio streaming (Issue #17, M3)
 *
 * Pure double-buffer state machine (gap-free refill) plus the MMIO wrapper
 * that wires it to the BCM2711 DMA controller and the I2S peripheral.
 */

#include "audio_stream.h"
#include "dma.h"
#include <stdint.h>

/* ===================================================================== *
 * Pure double-buffer state machine (compiled on the host too)
 * ===================================================================== */

void audio_dbuf_init(audio_dbuf_t *d, int16_t *a, int16_t *b, uint32_t frames,
                     audio_fill_fn fill, void *ctx)
{
    d->buf[0]      = a;
    d->buf[1]      = b;
    d->frames      = frames;
    d->fill        = fill;
    d->ctx         = ctx;
    d->last_played = 0;       /* DMA starts on buffer 0 */
}

void audio_dbuf_prime(audio_dbuf_t *d)
{
    d->fill(d->buf[0], d->frames, d->ctx);
    d->fill(d->buf[1], d->frames, d->ctx);
    d->last_played = 0;
}

void audio_dbuf_service(audio_dbuf_t *d, int now_playing)
{
    /* On the transition onto `now_playing`, the DMA has just finished the
     * other buffer, which is now safe to refill with the next chunk. */
    if (now_playing != d->last_played) {
        int freed = now_playing ^ 1;
        d->fill(d->buf[freed], d->frames, d->ctx);
        d->last_played = now_playing;
    }
}

/* ===================================================================== *
 * MMIO streaming driver (SoC only)
 * ===================================================================== */
#ifndef HOSTTEST

#include "i2s.h"

#define PCM_BASE     0xFE203000UL
#define PCM_CS_A     (*(volatile uint32_t *)(PCM_BASE + 0x00))
#define PCM_FIFO_PA  (PCM_BASE + 0x04)
#define PCM_DREQ_A   (*(volatile uint32_t *)(PCM_BASE + 0x14))
#define CS_DMAEN     (1u << 9)

#define AUDIO_CHANNEL 5

/* Static, 32-byte-aligned backing storage (DMA-accessible RAM). */
static int16_t  g_buf_a[AUDIO_MAX_FRAMES * 2];
static int16_t  g_buf_b[AUDIO_MAX_FRAMES * 2];
static dma_cb_t g_cb_a;
static dma_cb_t g_cb_b;

void audio_stream_init(audio_stream_t *s, uint32_t rate, uint32_t frames,
                       audio_fill_fn fill, void *ctx)
{
    if (frames > AUDIO_MAX_FRAMES)
        frames = AUDIO_MAX_FRAMES;
    s->frames  = frames;
    s->channel = AUDIO_CHANNEL;

    audio_dbuf_init(&s->db, g_buf_a, g_buf_b, frames, fill, ctx);

    /* I2S as master at `rate`, then enable its DMA request generation and a
     * TX FIFO DREQ threshold. */
    i2s_init(rate);
    PCM_DREQ_A = (0x10u << 16) | 0x30u;     /* TX panic / DREQ thresholds */
    PCM_CS_A |= CS_DMAEN;

    /* Two control blocks, each streaming one buffer to the FIFO, linked in a
     * ring so the DMA loops forever. */
    uint32_t len  = frames * 2u * sizeof(int16_t);
    uint32_t fifo = dma_bus_periph(PCM_FIFO_PA);
    uint32_t a_bus = dma_bus_mem((uint64_t)(uintptr_t)&g_cb_a);
    uint32_t b_bus = dma_bus_mem((uint64_t)(uintptr_t)&g_cb_b);
    dma_audio_cb_init(&g_cb_a, dma_bus_mem((uint64_t)(uintptr_t)g_buf_a), fifo, len, b_bus);
    dma_audio_cb_init(&g_cb_b, dma_bus_mem((uint64_t)(uintptr_t)g_buf_b), fifo, len, a_bus);

    dma_init_channel(s->channel);
}

void audio_stream_start(audio_stream_t *s)
{
    audio_dbuf_prime(&s->db);
    dma_start(s->channel, dma_bus_mem((uint64_t)(uintptr_t)&g_cb_a));
}

void audio_stream_service(audio_stream_t *s)
{
    /* Which control block (buffer) is the DMA currently executing? */
    uint32_t cur = dma_current_cb(s->channel);
    int now = (cur == dma_bus_mem((uint64_t)(uintptr_t)&g_cb_b)) ? 1 : 0;
    audio_dbuf_service(&s->db, now);
}

void audio_stream_stop(audio_stream_t *s)
{
    dma_stop(s->channel);
    PCM_CS_A &= ~CS_DMAEN;
}

#endif /* !HOSTTEST */
