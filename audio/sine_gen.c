/* audio/sine_gen.c - sine-tone test generator (Issue #18, M3)
 *
 * A phase-accumulator oscillator over a precomputed one-cycle 256-entry sine
 * table.  inc = freq * 2^32 / rate, so phase wraps once per output cycle; the
 * top 8 bits of the phase index the table.  Frequency changes only update the
 * increment, so the phase (and therefore the waveform) stays continuous: no
 * click on a frequency change.
 */

#include "sine_gen.h"
#include <stdint.h>

/* One full cycle, amplitude +/-32767.  round(32767 * sin(2*pi*i/256)). */
const int16_t sine_table[SINE_TABLE_SIZE] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

static uint32_t phase_inc(uint32_t freq, uint32_t rate)
{
    return (uint32_t)(((uint64_t)freq << 32) / (rate ? rate : 1u));
}

void sine_gen_init(sine_gen_t *g, uint32_t freq, uint32_t rate)
{
    g->phase     = 0;
    g->inc       = phase_inc(freq, rate);
    g->amplitude = 32767;
}

void sine_gen_set_freq(sine_gen_t *g, uint32_t freq, uint32_t rate)
{
    g->inc = phase_inc(freq, rate);   /* phase preserved -> no click */
}

void sine_gen_set_amplitude(sine_gen_t *g, uint16_t amplitude)
{
    g->amplitude = amplitude > 32767 ? 32767 : amplitude;
}

int16_t sine_gen_next(sine_gen_t *g)
{
    uint32_t idx = g->phase >> 24;                 /* top 8 bits -> 256 */
    g->phase += g->inc;
    int32_t s = sine_table[idx & (SINE_TABLE_SIZE - 1)];
    return (int16_t)((s * (int32_t)g->amplitude) >> 15);
}

void sine_gen_fill(sine_gen_t *g, int16_t *dst, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; i++) {
        int16_t s = sine_gen_next(g);
        dst[2 * i]     = s;
        dst[2 * i + 1] = s;
    }
}

/* ===================================================================== *
 * High-level control over the DMA streaming backend (SoC only)
 * ===================================================================== */
#ifndef HOSTTEST

#include "audio_stream.h"

#define SINE_RATE 48000u

static sine_gen_t     g_gen;
static audio_stream_t g_stream;

static void sine_fill_cb(int16_t *dst, uint32_t frames, void *ctx)
{
    (void)ctx;
    sine_gen_fill(&g_gen, dst, frames);
}

void audio_sine_start(uint32_t freq)
{
    sine_gen_init(&g_gen, freq, SINE_RATE);
    audio_stream_init(&g_stream, SINE_RATE, 256, sine_fill_cb, (void *)0);
    audio_stream_start(&g_stream);
}

void audio_sine_service(void)
{
    audio_stream_service(&g_stream);
}

void audio_sine_stop(void)
{
    audio_stream_stop(&g_stream);
}

void audio_sine_set_freq(uint32_t freq)
{
    sine_gen_set_freq(&g_gen, freq, SINE_RATE);   /* clean, phase-continuous */
}

void audio_sine_set_amplitude(uint16_t amplitude)
{
    sine_gen_set_amplitude(&g_gen, amplitude);
}

#endif /* !HOSTTEST */
