/* sdk/lib/tessera_param.c - drain the host parameter queue (Issue #38).
 *
 * The consumer half of the lock-free SPSC parameter queue the host maps into
 * the plugin.  Acquire/release ordering (LDAR/STLR via the compiler atomics)
 * makes it wait-free and real-time safe.  No libc, no allocation. */

#include "tessera.h"

int tessera_param_queue_read(tessera_param_queue_t *q, uint32_t *id, float *value)
{
    if (!q || __atomic_load_n(&q->magic, __ATOMIC_ACQUIRE) != TESSERA_PARAM_QUEUE_MAGIC)
        return 0;

    tessera_param_event_t *ev =
        (tessera_param_event_t *)((unsigned char *)q + sizeof(tessera_param_queue_t));

    uint32_t t = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);   /* we own  */
    uint32_t h = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);   /* peer    */
    if (h == t)
        return 0;                        /* empty */

    tessera_param_event_t e = ev[t & q->mask];
    __atomic_store_n(&q->tail, t + 1u, __ATOMIC_RELEASE);

    if (id)
        *id = e.id;
    if (value) {
        union { uint32_t u; float f; } cvt;
        cvt.u = e.bits;
        *value = cvt.f;
    }
    return 1;
}
