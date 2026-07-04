/* arch/arm64/arp.h - arpeggiator / step-sequencer node (Theme C, issue #116)
 *
 * Turns held notes into a stream of timed note events locked to the master
 * transport (#114): on each step boundary it releases the previous note and
 * sounds the next one chosen from the held set by the arp mode (up / down /
 * up-down / random).  The emitted events feed a downstream synth through the
 * ABI v1.1 event surface (#124).  It is monophonic (one note sounds at a time),
 * pure integer, and unit-tested on the host (make test-arm-arp).
 */

#ifndef ARM64_ARP_H
#define ARM64_ARP_H

#include <stdint.h>

#define ARP_MAX_NOTES 16u

typedef enum { ARP_UP = 0, ARP_DOWN, ARP_UPDOWN, ARP_RANDOM } arp_mode_t;

/* An emitted note event: on=1 note-on, on=0 note-off. */
typedef struct {
    uint8_t on;
    uint8_t note;
    uint8_t vel;
} arp_event_t;

typedef struct {
    uint8_t  held[ARP_MAX_NOTES];  /* held notes, kept sorted ascending */
    uint32_t n_held;

    uint32_t mode;                 /* arp_mode_t                        */
    uint32_t step_ticks;           /* transport ticks per arp step      */
    uint8_t  vel;                  /* velocity for emitted note-ons     */
    uint32_t enabled;

    int64_t  cur_step;             /* last emitted step index, -1 = none */
    int      playing;              /* currently-sounding note, -1 = none */
} arp_t;

/* Reset.  `step_ticks` is the arp step length in transport ticks (e.g. TP_PPQ/4
 * for 1/16 notes at TP_PPQ = 96), `vel` the note-on velocity. */
void arp_init(arp_t *a, arp_mode_t mode, uint32_t step_ticks, uint8_t vel);

void arp_set_mode(arp_t *a, arp_mode_t mode);
void arp_set_step(arp_t *a, uint32_t step_ticks);
void arp_enable(arp_t *a, int on);   /* off silences any sounding note */

/* Held-note set (note-on with vel 0 is treated as note-off, MIDI convention). */
void arp_note_on(arp_t *a, uint8_t note);
void arp_note_off(arp_t *a, uint8_t note);

/* Advance to the transport's absolute tick position and emit the resulting
 * events into `out` (capacity `max`): at a step boundary, a note-off for the
 * previous note (if any) then a note-on for this step's note.  Returns the
 * number of events written (0, 1, or 2).  Call once per block. */
int arp_run(arp_t *a, int64_t abs_tick, arp_event_t *out, int max);

/* The held-note index this step selects under the current mode (exposed for
 * testing the patterns). */
uint32_t arp_step_index(const arp_t *a, int64_t step);

#endif /* ARM64_ARP_H */
