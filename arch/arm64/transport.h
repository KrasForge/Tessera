/* arch/arm64/transport.h - master musical transport and clock (Theme C, #114)
 *
 * Time-aware effects (tempo-synced delay, LFOs, arpeggiators) need a shared
 * musical clock.  The transport is that clock: a tempo, a time signature, a
 * running bar/beat/tick position advanced from the audio-core cadence, and
 * play/stop - published to plugins each block as the ABI v1.1 transport
 * snapshot (issue #124).  It can free-run at its own tempo or slave to an
 * external MIDI clock, and it can emit MIDI clock to downstream gear.
 *
 * Pure fixed-point integer math (no floating point on the audio path): the
 * position is advanced by a Q32 tick accumulator.  Unit-tested on the host
 * (make test-arm-transport).
 */

#ifndef ARM64_TRANSPORT_H
#define ARM64_TRANSPORT_H

#include <stdint.h>

/* Internal resolution: ticks per quarter note.  Divisible by 24 so MIDI clock
 * (24 PPQN) maps to a whole number of internal ticks. */
#define TP_PPQ        96u
#define TP_MIDI_PPQN  24u

/* transport flags (snapshot) - value matches TESSERA_TRANSPORT_PLAYING. */
#define TP_PLAYING    1u

/* Snapshot published to plugins each block.  Field-for-field identical to the
 * SDK's tessera_transport_t (sdk/tessera.h), so the host copies it straight
 * into each plugin's event queue. */
typedef struct {
    uint32_t flags;       /* TP_PLAYING when running          */
    uint32_t tempo_mbpm;  /* tempo in milli-BPM (120000=120)  */
    uint32_t bar;         /* current bar  (0-based)           */
    uint32_t beat;        /* current beat (0-based)           */
    uint32_t tick;        /* ticks into the current beat      */
    uint32_t ppq;         /* ticks per quarter note (TP_PPQ)  */
} transport_snapshot_t;

typedef struct {
    uint32_t sr;
    uint32_t tempo_mbpm;
    uint32_t num, den;        /* time signature: num beats of 1/den per bar */
    uint32_t playing;

    uint32_t bar, beat, tick; /* position; tick in [0, ticks_per_beat)      */

    uint64_t tick_rem;        /* exact sub-tick remainder accumulator, < 60000*sr */

    uint32_t clkout_acc;      /* internal ticks since the last MIDI clock out */
    uint32_t clock_out;       /* pending 0xF8 pulses to emit                  */

    uint32_t clocked;         /* 1 once slaved to an external MIDI clock      */
} transport_t;

/* Reset to bar 0, stopped, 4/4, `tempo_mbpm` at sample rate `sr`. */
void transport_init(transport_t *t, uint32_t sr, uint32_t tempo_mbpm);

void transport_set_tempo(transport_t *t, uint32_t tempo_mbpm);
void transport_set_timesig(transport_t *t, uint32_t num, uint32_t den);

/* Transport control (also the MIDI real-time messages). */
void transport_start(transport_t *t);     /* 0xFA: rewind to 0 and play */
void transport_continue(transport_t *t);  /* 0xFB: play from here        */
void transport_stop(transport_t *t);      /* 0xFC: pause                 */

/* Advance the position by `n_frames` at the current tempo (call once per audio
 * block).  Accumulates MIDI-clock-out pulses. */
void transport_advance(transport_t *t, uint32_t n_frames);

/* Slave to an incoming MIDI clock (0xF8): advance one 24-PPQN tick and, given
 * the frames elapsed since the previous clock, estimate and set the tempo. */
void transport_midi_clock_in(transport_t *t, uint32_t frames_since_last);

/* Drain one pending MIDI-clock-out pulse (send 0xF8 for each).  Returns 1 while
 * pulses remain, 0 when none. */
int transport_clock_out(transport_t *t);

/* Fill the plugin-facing snapshot. */
void transport_snapshot(const transport_t *t, transport_snapshot_t *out);

/* Ticks per beat for the current time signature (TP_PPQ * 4 / den). */
uint32_t transport_ticks_per_beat(const transport_t *t);

/* True if `byte` is a MIDI System Real-Time status (0xF8..0xFF). */
static inline int midi_is_realtime(uint8_t byte) { return byte >= 0xF8u; }

#endif /* ARM64_TRANSPORT_H */
