/* arch/arm64/param_queue.c - lock-free parameter queue (Issue #30, M7) */

#include "param_queue.h"

void pq_init(param_queue_t *q, uint32_t capacity)
{
    q->capacity = capacity;
    q->mask     = capacity - 1u;
    q->_pad     = 0;
    q->head     = 0;
    q->tail     = 0;
    __atomic_store_n(&q->magic, PQ_MAGIC, __ATOMIC_RELEASE);
}

int pq_push(param_queue_t *q, uint32_t id, uint32_t bits)
{
    param_event_t *buf = pq_data(q);
    uint32_t h = __atomic_load_n(&q->head, __ATOMIC_RELAXED);   /* we own  */
    uint32_t t = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);   /* peer    */

    if (h - t >= q->capacity)
        return 0;                       /* full */

    buf[h & q->mask].id   = id;
    buf[h & q->mask].bits = bits;
    __atomic_store_n(&q->head, h + 1u, __ATOMIC_RELEASE);
    return 1;
}

int pq_pop(param_queue_t *q, uint32_t *id, uint32_t *bits)
{
    if (__atomic_load_n(&q->magic, __ATOMIC_ACQUIRE) != PQ_MAGIC)
        return 0;

    param_event_t *buf = pq_data(q);
    uint32_t t = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);   /* we own  */
    uint32_t h = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);   /* peer    */

    if (h == t)
        return 0;                       /* empty */

    *id   = buf[t & q->mask].id;
    *bits = buf[t & q->mask].bits;
    __atomic_store_n(&q->tail, t + 1u, __ATOMIC_RELEASE);
    return 1;
}

uint32_t pq_count(const param_queue_t *q)
{
    uint32_t h = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    return h - t;
}
