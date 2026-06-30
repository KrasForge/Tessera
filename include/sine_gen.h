/* include/sine_gen.h - sine-tone test generator (Issue #18, M3)
 *
 * The audio "hello world": a phase-accumulator oscillator over a precomputed
 * 256-entry / one-cycle 16-bit sine table.  It is the in-kernel test signal
 * that exercises the I2S (issue #16) and DMA (issue #17) backends, and the
 * latency/glitch reference.
 *
 * Changing the frequency keeps the running phase, so a frequency change is
 * waveform-continuous (no click).  The generator itself is pure and
 * host-tested; audio_sine_* wires it to a backend.
 */

#ifndef SINE_GEN_H
#define SINE_GEN_H

#include <stdint.h>

/* Number of entries in the one-cycle table. */
#define SINE_TABLE_SIZE 256

/* The precomputed 16-bit sine table (one full cycle, +/-32767). */
extern const int16_t sine_table[SINE_TABLE_SIZE];

/* Oscillator state. */
typedef struct {
    uint32_t phase;     /* 32-bit phase accumulator (wraps per cycle)   */
    uint32_t inc;       /* phase increment per sample                   */
    uint16_t amplitude; /* 0..32767                                     */
} sine_gen_t;

/* Initialise to `freq` Hz at sample rate `rate`, full amplitude. */
void sine_gen_init(sine_gen_t *g, uint32_t freq, uint32_t rate);

/* Set the frequency at runtime, preserving the phase (clean transition). */
void sine_gen_set_freq(sine_gen_t *g, uint32_t freq, uint32_t rate);

/* Set the amplitude (0..32767). */
void sine_gen_set_amplitude(sine_gen_t *g, uint16_t amplitude);

/* Produce one mono sample and advance the phase. */
int16_t sine_gen_next(sine_gen_t *g);

/* Fill `frames` interleaved stereo sample pairs (2*frames int16s). */
void sine_gen_fill(sine_gen_t *g, int16_t *dst, uint32_t frames);

/* ---- high-level control (MMIO backend) ------------------------------ */

/* Start a tone on the DMA streaming backend at `freq` Hz (48 kHz). */
void audio_sine_start(uint32_t freq);

/* Service the backend (refill) - call from a loop until the GIC lands (M4). */
void audio_sine_service(void);

/* Stop the tone. */
void audio_sine_stop(void);

/* Runtime control (callable from a test syscall or UART command). */
void audio_sine_set_freq(uint32_t freq);
void audio_sine_set_amplitude(uint16_t amplitude);

#endif /* SINE_GEN_H */
