/* arch/arm64/midi.c - MIDI parser and event ring (Issue #31, M7) */

#include "midi.h"

void midi_parser_init(midi_parser_t *p)
{
    p->status   = 0;
    p->expected = 0;
    p->ndata    = 0;
    p->data[0]  = p->data[1] = 0;
}

/* Number of data bytes a channel-voice status byte carries. */
static uint8_t data_len(uint8_t status)
{
    switch (status & 0xF0u) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    case 0xC0: case 0xD0:                                  return 1;
    default:                                               return 0;
    }
}

/* Assemble a completed channel-voice message into *out.  Returns 1 for a
 * supported event (NOTE_ON / NOTE_OFF / CC), 0 otherwise. */
static int emit(const midi_parser_t *p, midi_event_t *out)
{
    uint8_t hi = p->status & 0xF0u;
    uint8_t ch = p->status & 0x0Fu;

    if (hi == 0x90) {
        /* Note On; velocity 0 is the conventional Note Off. */
        out->type    = (p->data[1] == 0) ? MIDI_NOTE_OFF : MIDI_NOTE_ON;
        out->channel = ch;
        out->data1   = p->data[0];
        out->data2   = p->data[1];
        return 1;
    }
    if (hi == 0x80) {
        out->type    = MIDI_NOTE_OFF;
        out->channel = ch;
        out->data1   = p->data[0];
        out->data2   = p->data[1];
        return 1;
    }
    if (hi == 0xB0) {
        out->type    = MIDI_CC;
        out->channel = ch;
        out->data1   = p->data[0];
        out->data2   = p->data[1];
        return 1;
    }
    return 0;       /* aftertouch / program / pitch-bend: parsed, not emitted */
}

int midi_parse_byte(midi_parser_t *p, uint8_t byte, midi_event_t *out)
{
    if (byte >= 0xF8u)
        return 0;                       /* System Real-Time: ignore, no effect */

    if (byte >= 0x80u) {                /* status byte */
        if (byte >= 0xF0u) {
            /* System Common / Exclusive: cancels running status. */
            p->status   = 0;
            p->expected = 0;
            p->ndata    = 0;
            return 0;
        }
        p->status   = byte;             /* channel-voice status */
        p->expected = data_len(byte);
        p->ndata    = 0;
        return 0;
    }

    /* data byte */
    if (p->status == 0)
        return 0;                       /* orphan data byte */

    p->data[p->ndata++] = byte;
    if (p->ndata < p->expected)
        return 0;

    p->ndata = 0;                       /* keep status for running status */
    return emit(p, out);
}

/* ---- lock-free SPSC event ring ---- */

static midi_event_t *ring_data(midi_ring_t *r)
{
    return (midi_event_t *)((unsigned char *)r + sizeof(midi_ring_t));
}

void midi_ring_init(midi_ring_t *r, uint32_t capacity)
{
    r->capacity = capacity;
    r->mask     = capacity - 1u;
    r->_pad     = 0;
    r->head     = 0;
    r->tail     = 0;
    __atomic_store_n(&r->magic, MIDI_MAGIC, __ATOMIC_RELEASE);
}

int midi_ring_push(midi_ring_t *r, const midi_event_t *e)
{
    midi_event_t *buf = ring_data(r);
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    if (h - t >= r->capacity)
        return 0;
    buf[h & r->mask] = *e;
    __atomic_store_n(&r->head, h + 1u, __ATOMIC_RELEASE);
    return 1;
}

int midi_ring_pop(midi_ring_t *r, midi_event_t *e)
{
    if (__atomic_load_n(&r->magic, __ATOMIC_ACQUIRE) != MIDI_MAGIC)
        return 0;
    midi_event_t *buf = ring_data(r);
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    if (h == t)
        return 0;
    *e = buf[t & r->mask];
    __atomic_store_n(&r->tail, t + 1u, __ATOMIC_RELEASE);
    return 1;
}

uint32_t midi_ring_count(const midi_ring_t *r)
{
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    return h - t;
}
