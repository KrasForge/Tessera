/* arch/arm64/spsc_ring.h - lock-free single-producer/single-consumer ring
 *                          (Issue #21, M4)
 *
 * The audio path must never take a lock that a non-realtime task could hold:
 * a priority inversion there is an audible dropout.  The host process (on
 * CPU1-3) produces interleaved stereo samples into this ring; the audio
 * thread (pinned to CPU0) consumes them to refill its DMA buffer.  With a
 * single producer and a single consumer the ring needs no lock at all - just
 * one acquire/release-ordered index owned by each side.
 *
 * The whole module is pure C (GCC atomic builtins, available freestanding and
 * on the host), so the cross-core ordering is unit-tested on the host with a
 * real producer/consumer thread pair (make test-arm-smp).
 */

#ifndef ARM64_SPSC_RING_H
#define ARM64_SPSC_RING_H

#include <stdint.h>

/* A ring over int16 samples (interleaved L/R for stereo).  `cap` is a power of
 * two; one slot is kept empty to distinguish full from empty, so the usable
 * capacity is cap-1 samples. */
typedef struct {
    int16_t *buf;
    uint32_t cap;        /* power of two            */
    uint32_t mask;       /* cap - 1                 */
    uint32_t head;       /* producer index (monotonic, masked on use) */
    uint32_t tail;       /* consumer index          */
} spsc_ring_t;

/* Initialise the ring over `storage` of `cap_pow2` samples (must be a power
 * of two). */
void spsc_init(spsc_ring_t *r, int16_t *storage, uint32_t cap_pow2);

/* Producer: copy up to `n` samples from `src`; returns the number written
 * (less than n only when the ring is full). */
uint32_t spsc_write(spsc_ring_t *r, const int16_t *src, uint32_t n);

/* Consumer: copy up to `n` samples into `dst`; returns the number read (less
 * than n only when the ring is empty). */
uint32_t spsc_read(spsc_ring_t *r, int16_t *dst, uint32_t n);

/* Readable / writable sample counts (snapshot). */
uint32_t spsc_available(const spsc_ring_t *r);
uint32_t spsc_space(const spsc_ring_t *r);

#endif /* ARM64_SPSC_RING_H */
