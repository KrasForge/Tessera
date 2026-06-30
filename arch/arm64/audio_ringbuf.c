/* arch/arm64/audio_ringbuf.c - shared-memory audio ring buffer (Issue #25) */

#include "audio_ringbuf.h"

void arb_init(audio_ring_hdr_t *h, uint32_t capacity)
{
    h->capacity    = capacity;
    h->mask        = capacity - 1u;
    h->frame_words = 2u;          /* stereo */
    h->write_idx   = 0;
    h->read_idx    = 0;
    h->overflow    = 0;
    h->underflow   = 0;
    h->producer_state = ARB_PRODUCER_ALIVE;
    h->_reserved   = 0;
    /* Publish the magic last, with a release, so a consumer that sees a valid
     * magic also sees the initialised fields. */
    __atomic_store_n(&h->magic, ARB_MAGIC, __ATOMIC_RELEASE);
}

uint32_t arb_write(audio_ring_hdr_t *h, const float *in_f, uint32_t n_frames)
{
    const uint32_t *in = (const uint32_t *)in_f;     /* float32 bit-copy */
    uint32_t *buf  = arb_data(h);
    uint32_t mask = h->mask;
    uint32_t w = __atomic_load_n(&h->write_idx, __ATOMIC_RELAXED);  /* we own */
    uint32_t r = __atomic_load_n(&h->read_idx,  __ATOMIC_ACQUIRE);  /* peer   */

    uint32_t space = h->capacity - (w - r);
    uint32_t n = n_frames < space ? n_frames : space;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (w + i) & mask;
        buf[slot * 2u + 0] = in[i * 2u + 0];
        buf[slot * 2u + 1] = in[i * 2u + 1];
    }

    __atomic_store_n(&h->write_idx, w + n, __ATOMIC_RELEASE);

    if (n < n_frames)
        h->overflow += (n_frames - n);
    return n;
}

uint32_t arb_read(audio_ring_hdr_t *h, float *out_f, uint32_t n_frames)
{
    uint32_t *out = (uint32_t *)out_f;               /* float32 bit-copy */

    /* Crash resilience: an uninitialised or corrupted region yields silence. */
    if (__atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) != ARB_MAGIC) {
        for (uint32_t i = 0; i < n_frames * 2u; i++)
            out[i] = 0u;                             /* 0u == 0.0f bits */
        h->underflow += n_frames;
        return 0;
    }

    uint32_t *buf  = arb_data(h);
    uint32_t mask = h->mask;
    uint32_t r = __atomic_load_n(&h->read_idx,  __ATOMIC_RELAXED);  /* we own */
    uint32_t w = __atomic_load_n(&h->write_idx, __ATOMIC_ACQUIRE);  /* peer   */

    uint32_t avail = w - r;
    /* Impossible amount => a producer crashed mid-update or wrote garbage;
     * resynchronise to the published write index and emit silence. */
    if (avail > h->capacity) {
        __atomic_store_n(&h->read_idx, w, __ATOMIC_RELEASE);
        for (uint32_t i = 0; i < n_frames * 2u; i++)
            out[i] = 0u;
        h->underflow += n_frames;
        return 0;
    }

    uint32_t n = n_frames < avail ? n_frames : avail;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (r + i) & mask;
        out[i * 2u + 0] = buf[slot * 2u + 0];
        out[i * 2u + 1] = buf[slot * 2u + 1];
    }
    /* Fill the shortfall with silence rather than stale audio. */
    for (uint32_t i = n; i < n_frames; i++) {
        out[i * 2u + 0] = 0u;
        out[i * 2u + 1] = 0u;
    }

    __atomic_store_n(&h->read_idx, r + n, __ATOMIC_RELEASE);

    if (n < n_frames)
        h->underflow += (n_frames - n);
    return n;
}

uint32_t arb_available(const audio_ring_hdr_t *h)
{
    uint32_t w = __atomic_load_n(&h->write_idx, __ATOMIC_ACQUIRE);
    uint32_t r = __atomic_load_n(&h->read_idx,  __ATOMIC_ACQUIRE);
    uint32_t avail = w - r;
    return avail > h->capacity ? 0 : avail;
}

uint32_t arb_space(const audio_ring_hdr_t *h)
{
    return h->capacity - arb_available(h);
}
