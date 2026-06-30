/* arch/arm64/midi.h - MIDI parser and event ring (Issue #31, M7)
 *
 * Live keyboard input.  Bytes arrive from the DIN-5 MIDI driver (UART3 at
 * 31250 baud, drivers/midi_uart.c) and are fed one at a time to a small state
 * machine that emits typed events - NOTE_ON / NOTE_OFF / CC - which are then
 * handed to the host through a lock-free ring buffer (the same SPSC pattern as
 * the audio and parameter buffers).
 *
 * The parser handles MIDI running status (a stream of notes with the status
 * byte sent only once) and the velocity-0 Note On = Note Off convention, and
 * ignores System Real-Time bytes so they cannot corrupt a message in flight.
 * It is pure C and unit-tested on the host.
 */

#ifndef ARM64_MIDI_H
#define ARM64_MIDI_H

#include <stdint.h>

typedef enum {
    MIDI_NONE = 0,
    MIDI_NOTE_ON,
    MIDI_NOTE_OFF,
    MIDI_CC,
} midi_type_t;

typedef struct {
    uint8_t type;       /* midi_type_t                              */
    uint8_t channel;    /* 0..15                                    */
    uint8_t data1;      /* note number / CC number                  */
    uint8_t data2;      /* velocity / CC value                      */
} midi_event_t;

/* ---- byte-stream parser ---- */
typedef struct {
    uint8_t status;     /* running status byte, 0 if none           */
    uint8_t expected;   /* data bytes the current status needs      */
    uint8_t ndata;      /* data bytes collected so far              */
    uint8_t data[2];
} midi_parser_t;

void midi_parser_init(midi_parser_t *p);

/* Feed one received byte.  Returns 1 and fills *out when a supported event
 * (NOTE_ON / NOTE_OFF / CC) completes, 0 otherwise. */
int midi_parse_byte(midi_parser_t *p, uint8_t byte, midi_event_t *out);

/* ---- lock-free SPSC event ring ---- */
#define MIDI_MAGIC 0x4944494du   /* 'MIDI' */

typedef struct {
    uint32_t magic;
    uint32_t capacity;  /* events (power of two) */
    uint32_t mask;
    uint32_t _pad;
    uint32_t head;      /* producer (driver), release */
    uint32_t tail;      /* consumer (host), release   */
    /* midi_event_t events[capacity] follow */
} midi_ring_t;

static inline uint64_t midi_ring_bytes(uint32_t capacity)
{
    return sizeof(midi_ring_t) + (uint64_t)capacity * sizeof(midi_event_t);
}

void midi_ring_init(midi_ring_t *r, uint32_t capacity);
int  midi_ring_push(midi_ring_t *r, const midi_event_t *e);   /* 1 ok, 0 full  */
int  midi_ring_pop(midi_ring_t *r, midi_event_t *e);          /* 1 ok, 0 empty */
uint32_t midi_ring_count(const midi_ring_t *r);

#endif /* ARM64_MIDI_H */
