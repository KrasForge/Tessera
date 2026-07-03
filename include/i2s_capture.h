/* include/i2s_capture.h - I2S capture ring buffer (Issue #83, M14)
 *
 * The software side of audio input.  The BCM2711 PCM peripheral, configured
 * for RX alongside its TX path (same BCLK/LRCLK, so capture is sample-locked
 * to playback), streams 16-bit stereo samples through a DMA channel into
 * memory; a completion interrupt hands each filled block to this ring, and the
 * audio graph's input node (issue #84) consumes from it.
 *
 * The producer is hardware-paced and can never be told to wait, so the ring's
 * contract on a full buffer is drop-the-oldest: the newest block always lands
 * and an overrun is counted, rather than stalling the I2S clock or the audio
 * core.  The consumer reads whole blocks; an empty ring reports underrun (the
 * input node substitutes silence).
 *
 * The ring is pure C (no MMIO), so the bit-exact capture, wrap-around, and
 * overrun/underrun behaviour are unit-tested on the host (make
 * test-arm-i2s-rx) and driven by a modelled source on QEMU (make
 * test-arm-i2s-rx-qemu); only the PCM/DMA register programming in i2s.c /
 * dma.c is SoC-only.
 */

#ifndef I2S_CAPTURE_H
#define I2S_CAPTURE_H

#include <stdint.h>

/* A ring of fixed-size stereo blocks over caller-provided int16 storage.
 * `storage` must hold `n_blocks * frames * 2` samples (interleaved L/R). */
typedef struct {
    int16_t *storage;
    uint32_t frames;        /* stereo frames per block                     */
    uint32_t n_blocks;      /* number of block slots                       */
    uint32_t head;          /* next slot to write (producer)               */
    uint32_t tail;          /* next slot to read (consumer)                */
    uint32_t count;         /* filled slots                                */
    uint64_t produced;      /* blocks accepted from the source             */
    uint64_t consumed;      /* blocks handed to the consumer               */
    uint64_t overruns;      /* blocks dropped because the ring was full    */
    uint64_t underruns;     /* consume calls that found the ring empty     */
} i2s_capture_t;

/* Bind the ring to `storage` (n_blocks * frames * 2 int16s) and reset it. */
void i2s_capture_init(i2s_capture_t *c, int16_t *storage,
                      uint32_t n_blocks, uint32_t frames);

/* Producer (DMA completion path): copy one block of `frames` stereo pairs
 * from `block` (2*frames int16s) into the ring.  Never fails - on a full ring
 * the oldest unread block is dropped and `overruns` is bumped so the newest
 * data always lands. */
void i2s_capture_produce(i2s_capture_t *c, const int16_t *block);

/* Consumer (input node): copy the next block into `out` (2*frames int16s).
 * Returns 1 if a block was available, or 0 if the ring was empty (in which
 * case `out` is zero-filled and `underruns` is bumped). */
int i2s_capture_consume(i2s_capture_t *c, int16_t *out);

/* Blocks currently available to the consumer. */
uint32_t i2s_capture_available(const i2s_capture_t *c);

#endif /* I2S_CAPTURE_H */
