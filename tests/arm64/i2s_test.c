/* tests/arm64/i2s_test.c - host unit tests for the I2S clock + waveform math
 * (Issue #16).
 *
 * The MMIO register sequence cannot run on the host, but the clock-divider
 * and sine-generation logic that determines the output frequency is pure C
 * and is exactly what runs on the SoC.  This pins down the "measured
 * frequency within 1% of target" acceptance criterion computationally.
 *
 * Build/run via:  make test-arm-i2s
 */

#include "i2s.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Relative error in parts-per-thousand, integer math. */
static uint32_t ppk(uint32_t actual, uint32_t target)
{
    uint32_t diff = actual > target ? actual - target : target - actual;
    return (uint32_t)(((uint64_t)diff * 1000u) / target);
}

static void test_clock(uint32_t rate)
{
    uint32_t frame = i2s_frame_bits(16);          /* 32 */
    uint32_t bclk  = rate * frame;
    pcm_div_t d = pcm_clock_divider(PLLD_HZ, bclk);
    uint32_t actual_bclk = pcm_actual_hz(PLLD_HZ, d);
    uint32_t actual_lrclk = actual_bclk / frame;

    char buf[96];
    snprintf(buf, sizeof buf, "%u Hz: LRCLK=%u (err %u/1000) within 1%%",
             rate, actual_lrclk, ppk(actual_lrclk, rate));
    CHECK(ppk(actual_lrclk, rate) <= 10, buf);     /* <= 1% */
}

int main(void)
{
    printf("=== Tessera I2S clock + waveform tests (issue #16) ===\n");

    CHECK(i2s_frame_bits(16) == 32, "16-bit stereo frame is 32 BCLK");

    test_clock(48000);
    test_clock(44100);

    /* Divider sanity: PLLD / (DIVI + DIVF/4096) reproduces the target. */
    pcm_div_t d = pcm_clock_divider(PLLD_HZ, 1536000);   /* 48 kHz * 32 */
    CHECK(d.divi == 325, "48 kHz divider integer part is 325");
    CHECK(ppk(pcm_actual_hz(PLLD_HZ, d), 1536000) <= 1,
          "48 kHz bit clock reproduced within 0.1%");

    /* Sine generator frequency: count sign changes over one second of
     * samples; frequency ~= sign_changes / 2. */
    const uint32_t rate = 48000, freq = 440;
    uint32_t phase = 0;
    int16_t prev = i2s_sine(&phase, freq, rate);
    int crossings = 0;
    for (uint32_t i = 1; i < rate; i++) {
        int16_t s = i2s_sine(&phase, freq, rate);
        if ((prev <= 0 && s > 0) || (prev >= 0 && s < 0))
            crossings++;
        prev = s;
    }
    uint32_t measured = (uint32_t)crossings / 2;
    char buf[96];
    snprintf(buf, sizeof buf, "440 Hz sine measured %u Hz within 1%%", measured);
    CHECK(ppk(measured, freq) <= 10, buf);

    /* Amplitude is non-trivial (the generator actually oscillates). */
    int16_t peak = 0;
    phase = 0;
    for (uint32_t i = 0; i < rate; i++) {
        int16_t s = i2s_sine(&phase, freq, rate);
        if (s > peak) peak = s;
    }
    CHECK(peak > 20000, "sine reaches a usable amplitude");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
