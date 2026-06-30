/* arch/arm64/cvgate.h - CV/Gate input (Issue #32, M7)
 *
 * Modular-synth control input: a Gate signal on a GPIO pin (rising edge = note
 * on, falling edge = note off) and a Pitch CV read through an external SPI ADC
 * (an MCP3208, 12-bit) at the Eurorack standard of 1V per octave.  The gate
 * edges and the sampled pitch are turned into the same event type as MIDI
 * (issue #31) and delivered on the same lock-free ring, tagged INPUT_SRC_CV so
 * the two sources coexist without interference.
 *
 * The ADC protocol decode and the 1V/oct scaling are pure and host-tested; the
 * SPI/GPIO bring-up lives in drivers/spi.c.
 */

#ifndef ARM64_CVGATE_H
#define ARM64_CVGATE_H

#include "midi.h"
#include <stdint.h>

/* ---- MCP3208 single-ended read protocol (pure) ---- */

/* Fill `tx[3]` with the SPI command to read single-ended channel 0..7. */
void mcp3208_command(uint8_t channel, uint8_t tx[3]);

/* Decode the 3 received bytes into the 12-bit conversion result (0..4095). */
uint16_t mcp3208_decode(const uint8_t rx[3]);

/* ---- 1V/octave scaling (pure) ---- */

typedef struct {
    uint16_t code_0v;         /* ADC code that corresponds to 0 V          */
    uint16_t codes_per_volt;  /* ADC codes per 1 V (== one octave)         */
    uint8_t  base_note;       /* MIDI note number at 0 V                   */
} cv_cal_t;

/* Convert a 12-bit ADC code to a MIDI note number using 1V/oct (12 semitones
 * per volt), rounded to the nearest semitone and clamped to 0..127. */
uint8_t cv_to_note(uint16_t code, const cv_cal_t *cal);

/* ---- gate/pitch runtime ---- */

typedef struct {
    cv_cal_t cal;
    int      gate;       /* last seen gate level (0/1)   */
    uint8_t  channel;    /* event channel                */
} cvgate_t;

void cvgate_init(cvgate_t *cg, const cv_cal_t *cal, uint8_t channel);

/* Build a CV note event (rising -> NOTE_ON velocity 127, falling -> NOTE_OFF),
 * source = INPUT_SRC_CV. */
void cvgate_make_event(const cvgate_t *cg, int rising, uint8_t note, midi_event_t *out);

/* Given the current gate level and the sampled 12-bit pitch code, emit a note
 * event into `ring` on a gate edge (using the pitch sampled at that edge).
 * Returns 1 if an event was pushed, 0 if the gate did not change. */
int cvgate_update(cvgate_t *cg, int gate_level, uint16_t pitch_code, midi_ring_t *ring);

#endif /* ARM64_CVGATE_H */
