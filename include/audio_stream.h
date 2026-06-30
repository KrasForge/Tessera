/* include/audio_stream.h - DMA double-buffered audio streaming (Issue #17, M3)
 *
 * Continuous 48 kHz / 16-bit stereo output without polling: a DMA ring of two
 * control blocks drains buffer A then B (and loops), while the CPU refills
 * whichever buffer the DMA is not currently reading via a fill callback.
 *
 * The double-buffer state machine (audio_dbuf_*) is pure and unit-tested on
 * the host to prove gap-free output; the audio_stream_* wrapper drives the
 * real DMA + I2S hardware.
 */

#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H

#include "dma.h"
#include <stdint.h>

/* Fill `frames` interleaved L/R sample pairs (2*frames int16s) into dst. */
typedef void (*audio_fill_fn)(int16_t *dst, uint32_t frames, void *ctx);

/* ---- pure double-buffer state machine (host-testable) --------------- */

typedef struct {
    int16_t      *buf[2];     /* the two stereo buffers   */
    uint32_t      frames;     /* frames per buffer        */
    audio_fill_fn fill;
    void         *ctx;
    int           last_played;/* buffer index DMA was last seen reading  */
} audio_dbuf_t;

void audio_dbuf_init(audio_dbuf_t *d, int16_t *a, int16_t *b, uint32_t frames,
                     audio_fill_fn fill, void *ctx);

/* Fill both buffers before streaming starts. */
void audio_dbuf_prime(audio_dbuf_t *d);

/* Called with the buffer index the DMA is now reading (0 or 1): refill the
 * buffer the DMA just left.  Idempotent within a buffer period. */
void audio_dbuf_service(audio_dbuf_t *d, int now_playing);

/* ---- streaming driver (MMIO) ---------------------------------------- */

/* Maximum frames per buffer (static backing storage). */
#define AUDIO_MAX_FRAMES 256

typedef struct {
    audio_dbuf_t db;
    int          channel;
    uint32_t     frames;
} audio_stream_t;

/* Set up I2S (rate) + DMA channel + the two-CB ring; `frames` per buffer
 * (<= AUDIO_MAX_FRAMES; 256 default, 128 for lower latency). */
void audio_stream_init(audio_stream_t *s, uint32_t rate, uint32_t frames,
                       audio_fill_fn fill, void *ctx);

/* Prime both buffers and start the DMA ring. */
void audio_stream_start(audio_stream_t *s);

/* Poll which buffer the DMA is draining and refill the other.  Until the M4
 * GIC/timer lands, this is called from a service loop instead of the DMA
 * completion interrupt. */
void audio_stream_service(audio_stream_t *s);

void audio_stream_stop(audio_stream_t *s);

#endif /* AUDIO_STREAM_H */
