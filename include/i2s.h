/* include/i2s.h - BCM2711 PCM/I2S audio output driver (Issue #16, M3)
 *
 * Drives an external I2S DAC (e.g. PCM5102) from the BCM2711 PCM peripheral,
 * configured as I2S master: the SoC generates BCLK and LRCLK and streams
 * 16-bit stereo samples.  This replaces the x86 AC97 backend
 * (kernel/audio_ac97.c) for the ARM target.
 *
 * The pure clock / sample-rate / waveform helpers below are also unit-tested
 * on the host; the MMIO routines run on the SoC (or against a mapped scratch
 * region in the QEMU smoke test).
 */

#ifndef I2S_H
#define I2S_H

#include <stdint.h>
#include <stddef.h>

/* ---- pure helpers (host-testable) ----------------------------------- */

/* PCM clock-manager fractional divider: DIVI (integer) + DIVF (n/4096). */
typedef struct {
    uint32_t divi;
    uint32_t divf;
} pcm_div_t;

/* Source clock for the PCM clock manager: PLLD is 500 MHz on the BCM2711. */
#define PLLD_HZ 500000000u

/* Bits per I2S frame for `bits`-per-sample stereo (2 channels). */
static inline uint32_t i2s_frame_bits(uint32_t bits) { return bits * 2u; }

/* Compute the DIVI/DIVF that bring src_hz down to target_hz (1-stage MASH). */
pcm_div_t pcm_clock_divider(uint32_t src_hz, uint32_t target_hz);

/* Actual frequency produced by a given divider (for verification/tests). */
uint32_t pcm_actual_hz(uint32_t src_hz, pcm_div_t d);

/* 16-point interpolated sine generator.  Advances *phase by one sample at
 * `freq` Hz for sample rate `rate`, returns the Q15-ish sample. */
int16_t i2s_sine(uint32_t *phase, uint32_t freq, uint32_t rate);

/* ---- driver API ----------------------------------------------------- */

/* Initialise the PCM peripheral as a 16-bit-stereo I2S master at `rate` Hz
 * (44100 or 48000), muxing GPIO 18/19/21 to ALT0 (BCLK/LRCLK/DOUT). */
void i2s_init(uint32_t rate);

/* Reconfigure the bit clock for a new sample rate. */
void i2s_set_sample_rate(uint32_t rate);

/* Write one stereo sample, blocking (bounded) until the TX FIFO has room. */
void i2s_write_stereo(int16_t left, int16_t right);

/* Write `count` interleaved L/R sample pairs from buf (2*count int16s). */
void i2s_write_samples(const int16_t *buf, size_t count);

/* Generate and play `count` sample pairs of a `freq` Hz sine (mono->stereo). */
void i2s_play_tone(uint32_t freq, uint32_t count);

/* ---- capture / RX (issue #83, M14) ---------------------------------- *
 *
 * The RX path shares the PCM block, its BCLK/LRCLK, and the sample clock with
 * TX, so captured audio is inherently sample-locked to playback.  Wire the
 * external ADC's data line to GPIO 20 (PCM_DIN, ALT0); see docs/hardware.md. */

/* Enable the RX channel and start capturing into the PCM RX FIFO.  Call after
 * i2s_init() (which brought up the shared clock and TX). */
void i2s_capture_enable(void);

/* Polled read of one stereo frame from the RX FIFO (bounded wait).  Returns 0
 * and fills *left/*right, or -1 if no sample arrived (e.g. under emulation). */
int i2s_read_stereo(int16_t *left, int16_t *right);

/* Start a DMA channel capturing the RX FIFO into `buf_bus` (a bus address of a
 * RAM buffer) as a `len_bytes` transfer, using `cb` (a RAM-resident control
 * block, passed by its own bus address `cb_bus`).  The channel chains the CB
 * to itself for continuous capture. */
void i2s_capture_dma_start(int channel, uint32_t cb_bus, void *cb,
                           uint32_t buf_bus, uint32_t len_bytes);

#endif /* I2S_H */
