/* arch/arm64/spsc_ring.c - lock-free SPSC ring (Issue #21, M4)
 *
 * Lamport's single-producer/single-consumer queue: the producer owns `head`
 * and the consumer owns `tail`.  Each side publishes its index with a
 * release store and observes the other's with an acquire load, which on
 * AArch64 lowers to STLR/LDAR.  That pairing is exactly what guarantees the
 * consumer sees the sample data before it sees the advanced head (and vice
 * versa) with no lock and no disabled interrupts on the audio path.
 */

#include "spsc_ring.h"

void spsc_init(spsc_ring_t *r, int16_t *storage, uint32_t cap_pow2)
{
    r->buf  = storage;
    r->cap  = cap_pow2;
    r->mask = cap_pow2 - 1u;
    r->head = 0;
    r->tail = 0;
}

uint32_t spsc_write(spsc_ring_t *r, const int16_t *src, uint32_t n)
{
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);   /* we own it */
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);   /* consumer  */

    uint32_t space = r->mask - (head - tail);   /* keep one slot empty */
    if (n > space)
        n = space;

    for (uint32_t i = 0; i < n; i++)
        r->buf[(head + i) & r->mask] = src[i];

    __atomic_store_n(&r->head, head + n, __ATOMIC_RELEASE);
    return n;
}

uint32_t spsc_read(spsc_ring_t *r, int16_t *dst, uint32_t n)
{
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);   /* we own it */
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);   /* producer  */

    uint32_t avail = head - tail;
    if (n > avail)
        n = avail;

    for (uint32_t i = 0; i < n; i++)
        dst[i] = r->buf[(tail + i) & r->mask];

    __atomic_store_n(&r->tail, tail + n, __ATOMIC_RELEASE);
    return n;
}

uint32_t spsc_available(const spsc_ring_t *r)
{
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    return head - tail;
}

uint32_t spsc_space(const spsc_ring_t *r)
{
    return r->mask - spsc_available(r);
}
