/* arch/arm64/audio_ringbuf.h - shared-memory audio ring buffer (Issue #25, M5)
 *
 * The README's data path: "audio moves between plugins via shared-memory ring
 * buffers - zero kernel involvement per block.  The kernel sets up the shared
 * region once; plugins read and write directly."
 *
 * The kernel allocates one physically-contiguous region and maps it read-write
 * into both the producer (plugin) and the consumer (host) at the SAME virtual
 * address.  After that, audio flows with no syscalls: the producer appends
 * float32 stereo frames and the consumer drains them, coordinating only
 * through two free-running indices published with release stores and read with
 * acquire loads (STLR/LDAR on AArch64) - a lock-free SPSC queue.
 *
 * The header and the data live together in the shared region:
 *
 *     +----------------------------+  offset 0
 *     | audio_ring_hdr_t           |
 *     +----------------------------+  sizeof(audio_ring_hdr_t)
 *     | float samples[capacity*2]  |  interleaved L,R,L,R,...
 *     +----------------------------+
 *
 * Everything here is pure C (GCC atomics), so the lock-free behaviour, the
 * overflow/underflow accounting, and the crash-resilient read path are
 * unit-tested on the host (make test-arm-ringbuf).
 */

#ifndef ARM64_AUDIO_RINGBUF_H
#define ARM64_AUDIO_RINGBUF_H

#include <stdint.h>
#include <stddef.h>

#define ARB_MAGIC 0x52427541u    /* 'AuBR' - marks a valid, initialised ring */

typedef struct {
    uint32_t magic;       /* ARB_MAGIC once initialised                    */
    uint32_t capacity;    /* frames the ring holds (power of two)          */
    uint32_t mask;        /* capacity - 1                                  */
    uint32_t frame_words; /* floats per frame (2 = stereo)                 */
    uint32_t write_idx;   /* producer index, free-running (release store)  */
    uint32_t read_idx;    /* consumer index, free-running (release store)  */
    uint32_t overflow;    /* frames dropped because the ring was full      */
    uint32_t underflow;   /* frames the consumer had to fill with silence  */
} audio_ring_hdr_t;

/* Total bytes a ring of `capacity` stereo frames needs (header + samples). */
static inline size_t arb_bytes(uint32_t capacity)
{
    return sizeof(audio_ring_hdr_t) + (size_t)capacity * 2u * sizeof(float);
}

/* The interleaved sample area immediately following the header.  Samples are
 * moved as raw 32-bit words (a float32 bit-copy) so the ring code needs no
 * floating-point registers and builds in the -mgeneral-regs-only kernel; the
 * public read/write API still speaks float* for callers' convenience. */
static inline uint32_t *arb_data(audio_ring_hdr_t *h)
{
    return (uint32_t *)((unsigned char *)h + sizeof(audio_ring_hdr_t));
}

/* Initialise a ring in place over a region of at least arb_bytes(capacity).
 * `capacity` must be a power of two. */
void arb_init(audio_ring_hdr_t *h, uint32_t capacity);

/* Producer: append up to `n_frames` interleaved stereo frames (2*n floats).
 * Returns the number of frames written; any shortfall (ring full) is dropped
 * and counted in `overflow`. */
uint32_t arb_write(audio_ring_hdr_t *h, const float *interleaved, uint32_t n_frames);

/* Consumer: read up to `n_frames` frames into `out` (2*n floats).  Returns the
 * number of real frames read; the remainder of `out` is filled with silence
 * and counted in `underflow`.  Crash-resilient: if the region is uninitialised
 * or the indices are impossible (a producer crashed mid-update), the read
 * yields silence and resynchronises instead of reading garbage. */
uint32_t arb_read(audio_ring_hdr_t *h, float *out, uint32_t n_frames);

/* Readable / writable frame counts (snapshot). */
uint32_t arb_available(const audio_ring_hdr_t *h);
uint32_t arb_space(const audio_ring_hdr_t *h);

#endif /* ARM64_AUDIO_RINGBUF_H */
