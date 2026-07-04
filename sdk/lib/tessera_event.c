/* sdk/lib/tessera_event.c - drain the host note-event queue (Plugin ABI v1.1,
 * issue #124).
 *
 * The consumer half of the lock-free SPSC event queue the host maps into a v1.1
 * plugin at TESSERA_EVENT_QUEUE_VA - the note/CC events and the transport
 * snapshot for the current block.  Acquire/release ordering makes the drain
 * wait-free and real-time safe.  A v1.0 plugin simply never reads it, which is
 * why v1.1 is backward compatible.  No libc, no allocation. */

#include "tessera.h"

int tessera_event_read(tessera_event_queue_t *q, tessera_note_event_t *ev)
{
    if (!q || __atomic_load_n(&q->magic, __ATOMIC_ACQUIRE) != TESSERA_EVENT_QUEUE_MAGIC)
        return 0;

    tessera_note_event_t *evs =
        (tessera_note_event_t *)((unsigned char *)q + sizeof(tessera_event_queue_t));

    uint32_t t = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);   /* we own  */
    uint32_t h = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);   /* peer    */
    if (h == t)
        return 0;                        /* empty */

    tessera_note_event_t e = evs[t & q->mask];
    __atomic_store_n(&q->tail, t + 1u, __ATOMIC_RELEASE);

    if (ev)
        *ev = e;
    return 1;
}

void tessera_transport_read(const tessera_event_queue_t *q, tessera_transport_t *out)
{
    if (!out)
        return;
    if (!q || __atomic_load_n(&q->magic, __ATOMIC_ACQUIRE) != TESSERA_EVENT_QUEUE_MAGIC) {
        out->flags = 0; out->tempo_mbpm = 0; out->bar = 0;
        out->beat = 0;  out->tick = 0;       out->ppq = 0;
        return;
    }
    /* The host refreshes the snapshot before dispatching the block, so a plain
     * read is consistent within process_block. */
    *out = q->transport;
}

/* ---- sample-accurate event delivery (v1.3, issue #199) ------------------- */

void tessera_event_split_init(tessera_event_split_t *sp,
                              tessera_event_queue_t *q, uint32_t block)
{
    sp->q         = q;
    sp->block     = block;
    sp->cursor    = 0;
    sp->have_look = 0;
    sp->done      = 0;
}

int tessera_event_split_next(tessera_event_split_t *sp,
                             uint32_t *start, uint32_t *len,
                             tessera_note_event_t *ev, int *have_event)
{
    if (sp->done)
        return 0;

    /* Pull one lookahead event if we do not already hold one. */
    if (!sp->have_look)
        sp->have_look = tessera_event_read(sp->q, &sp->look);

    uint32_t seg_start = sp->cursor;

    if (sp->have_look) {
        /* The lookahead event is the boundary for this segment.  Clamp its
         * offset into [cursor, block] so an out-of-order or out-of-range value
         * cannot run the cursor backwards or past the block end. */
        uint32_t off = sp->look.frame_offset;
        if (off > sp->block)  off = sp->block;
        if (off < sp->cursor) off = sp->cursor;

        *start = seg_start;
        *len   = off - seg_start;
        if (ev) *ev = sp->look;
        if (have_event) *have_event = 1;

        sp->cursor    = off;
        sp->have_look = 0;               /* consumed; applied at this boundary */
        return 1;
    }

    /* No more events: render the tail of the block. */
    *start = seg_start;
    *len   = (sp->block > sp->cursor) ? sp->block - sp->cursor : 0u;
    if (have_event) *have_event = 0;
    sp->cursor = sp->block;
    sp->done   = 1;
    return 1;
}
