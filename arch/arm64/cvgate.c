/* arch/arm64/cvgate.c - CV/Gate input (Issue #32, M7) */

#include "cvgate.h"

void mcp3208_command(uint8_t channel, uint8_t tx[3])
{
    channel &= 0x07u;
    tx[0] = (uint8_t)(0x06u | (channel >> 2));   /* start, single, D2 */
    tx[1] = (uint8_t)((channel & 0x03u) << 6);   /* D1, D0            */
    tx[2] = 0x00u;
}

uint16_t mcp3208_decode(const uint8_t rx[3])
{
    return (uint16_t)(((rx[1] & 0x0Fu) << 8) | rx[2]);   /* 12-bit result */
}

uint8_t cv_to_note(uint16_t code, const cv_cal_t *cal)
{
    int delta = (int)code - (int)cal->code_0v;
    int den   = cal->codes_per_volt ? (int)cal->codes_per_volt : 1;
    int num   = delta * 12;                       /* 12 semitones per volt */

    /* Round to the nearest semitone (handle the sign for symmetric rounding). */
    int semis = (num >= 0) ? (num + den / 2) / den
                           : -((-num + den / 2) / den);

    int note = (int)cal->base_note + semis;
    if (note < 0)   note = 0;
    if (note > 127) note = 127;
    return (uint8_t)note;
}

void cvgate_init(cvgate_t *cg, const cv_cal_t *cal, uint8_t channel)
{
    cg->cal     = *cal;
    cg->gate    = 0;
    cg->channel = channel;
}

void cvgate_make_event(const cvgate_t *cg, int rising, uint8_t note, midi_event_t *out)
{
    out->type    = rising ? MIDI_NOTE_ON : MIDI_NOTE_OFF;
    out->channel = cg->channel;
    out->data1   = note;
    out->data2   = rising ? 127 : 0;     /* gate has no velocity */
    out->source  = INPUT_SRC_CV;
}

int cvgate_update(cvgate_t *cg, int gate_level, uint16_t pitch_code, midi_ring_t *ring)
{
    gate_level = gate_level ? 1 : 0;
    if (gate_level == cg->gate)
        return 0;                        /* no edge */

    uint8_t note = cv_to_note(pitch_code, &cg->cal);
    midi_event_t e;
    cvgate_make_event(cg, gate_level, note, &e);
    cg->gate = gate_level;
    return midi_ring_push(ring, &e);
}
