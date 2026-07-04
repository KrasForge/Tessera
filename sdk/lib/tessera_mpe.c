/* sdk/lib/tessera_mpe.c - MPE / per-note expression decoder (Theme M17, #171).
 * See tessera.h.
 *
 * MIDI Polyphonic Expression puts each sounding note on its own channel, so the
 * channel's pitch bend, channel pressure, and CC 74 (timbre) belong to whatever
 * note is currently held on that channel.  This decoder tracks the active note
 * per channel and rewrites those channel-wide messages as per-note events the
 * synth can apply independently. */

#include "tessera.h"

#define CC_TIMBRE 74u   /* the MPE "third dimension" (timbre / brightness) */

void tessera_mpe_init(tessera_mpe_t *m)
{
    for (int i = 0; i < 16; i++) m->active_note[i] = -1;
}

static tessera_note_event_t mk(uint8_t type, uint8_t ch, uint8_t d1, uint8_t d2, int16_t val)
{
    tessera_note_event_t e;
    e.type = type; e.channel = ch; e.data1 = d1; e.data2 = d2; e.value = val; e._pad = 0;
    return e;
}

int tessera_mpe_feed(tessera_mpe_t *m, uint8_t status, uint8_t d1, uint8_t d2,
                     tessera_note_event_t *out, int max)
{
    uint8_t hi = status & 0xF0u;
    uint8_t ch = status & 0x0Fu;
    int n = 0;
    #define EMIT(ev) do { if (n < max) out[n] = (ev); n++; } while (0)

    switch (hi) {
    case 0x90:   /* Note On (velocity 0 is a Note Off) */
        if (d2 == 0) {
            m->active_note[ch] = -1;
            EMIT(mk(TESSERA_EV_NOTE_OFF, ch, d1, 0, 0));
        } else {
            m->active_note[ch] = (int8_t)d1;
            EMIT(mk(TESSERA_EV_NOTE_ON, ch, d1, d2, 0));
        }
        break;

    case 0x80:   /* Note Off */
        m->active_note[ch] = -1;
        EMIT(mk(TESSERA_EV_NOTE_OFF, ch, d1, d2, 0));
        break;

    case 0xE0: { /* Pitch Bend -> per-note PITCH for the channel's active note */
        int note = m->active_note[ch];
        if (note >= 0) {
            int bend = (int)d1 | ((int)d2 << 7);      /* 0..16383 */
            EMIT(mk(TESSERA_EV_PITCH, ch, (uint8_t)note, 0, (int16_t)(bend - 8192)));
        }
        break;
    }

    case 0xD0: { /* Channel Pressure -> per-note PRESSURE */
        int note = m->active_note[ch];
        if (note >= 0)
            EMIT(mk(TESSERA_EV_PRESSURE, ch, (uint8_t)note, d1, 0));
        break;
    }

    case 0xB0:   /* Control Change */
        if (d1 == CC_TIMBRE) {
            int note = m->active_note[ch];
            if (note >= 0)
                EMIT(mk(TESSERA_EV_TIMBRE, ch, (uint8_t)note, d2, 0));
        } else {
            EMIT(mk(TESSERA_EV_CC, ch, d1, d2, 0));   /* ordinary CC passes through */
        }
        break;

    default:
        break;   /* program change, poly aftertouch, etc.: not expression */
    }

    #undef EMIT
    return n;
}
