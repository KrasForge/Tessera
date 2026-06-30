/* arch/arm64/param_queue.h - lock-free parameter queue (Issue #30, M7)
 *
 * sys_plugin_set_param delivers control changes to a plugin through a small
 * lock-free single-producer/single-consumer queue in a shared page: the host
 * (producer) pushes (param_id, value) events; the plugin (consumer) drains
 * them at the top of each process_block.  Like the audio ring, indices are
 * published with release stores and read with acquire loads (STLR/LDAR), so
 * there is no lock on the audio path, and a value is delivered within one
 * block.
 *
 * The float value is carried as its 32-bit bit pattern so the queue code needs
 * no FP and builds in the -mgeneral-regs-only kernel; the plugin reinterprets
 * the bits as a float.  Pure C, unit-tested on the host.
 */

#ifndef ARM64_PARAM_QUEUE_H
#define ARM64_PARAM_QUEUE_H

#include <stdint.h>
#include <stddef.h>

#define PQ_MAGIC 0x51505141u    /* 'AQPQ' */

typedef struct {
    uint32_t id;       /* parameter id            */
    uint32_t bits;     /* float value, bit pattern */
} param_event_t;

typedef struct {
    uint32_t magic;
    uint32_t capacity; /* events (power of two)   */
    uint32_t mask;
    uint32_t _pad;
    uint32_t head;     /* producer index (release) */
    uint32_t tail;     /* consumer index (release) */
} param_queue_t;

static inline size_t pq_bytes(uint32_t capacity)
{
    return sizeof(param_queue_t) + (size_t)capacity * sizeof(param_event_t);
}

static inline param_event_t *pq_data(param_queue_t *q)
{
    return (param_event_t *)((unsigned char *)q + sizeof(param_queue_t));
}

/* Initialise a queue in place over a region of at least pq_bytes(capacity);
 * capacity must be a power of two. */
void pq_init(param_queue_t *q, uint32_t capacity);

/* Producer: enqueue (id, bits).  Returns 1 on success, 0 if the queue is full. */
int pq_push(param_queue_t *q, uint32_t id, uint32_t bits);

/* Consumer: dequeue into the id and bits outputs.  Returns 1 on success, 0 if
 * the queue is empty. */
int pq_pop(param_queue_t *q, uint32_t *id, uint32_t *bits);

/* Number of pending events. */
uint32_t pq_count(const param_queue_t *q);

#endif /* ARM64_PARAM_QUEUE_H */
