/* tests/arm64/sine_test.c - host unit tests for the sine generator (Issue #18).
 *
 * The oscillator is pure C, so its table accuracy, frequency, amplitude and -
 * the key acceptance - the click-free (phase-continuous) frequency change can
 * all be checked on the host.
 *
 * Build/run via:  make test-arm-sine
 */

#include "sine_gen.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static uint32_t ppk(int32_t a, int32_t b)
{
    int32_t diff = a > b ? a - b : b - a;
    if (b < 0) b = -b;
    return b ? (uint32_t)(((int64_t)diff * 1000) / b) : 0;
}

static uint32_t measure_freq(uint32_t freq, uint32_t rate)
{
    sine_gen_t g;
    sine_gen_init(&g, freq, rate);
    int16_t prev = sine_gen_next(&g);
    int crossings = 0;
    for (uint32_t i = 1; i < rate; i++) {
        int16_t s = sine_gen_next(&g);
        if ((prev <= 0 && s > 0) || (prev >= 0 && s < 0))
            crossings++;
        prev = s;
    }
    return (uint32_t)crossings / 2;
}

int main(void)
{
    printf("=== Tessera sine-generator tests (issue #18) ===\n");

    /* ---- table is a real one-cycle sine ---- */
    CHECK(sine_table[0] == 0, "table[0] = 0");
    CHECK(sine_table[64] == 32767, "table[64] = +full scale (90 deg)");
    CHECK(sine_table[128] == 0, "table[128] = 0 (180 deg)");
    CHECK(sine_table[192] == -32767, "table[192] = -full scale (270 deg)");

    /* ---- frequency accuracy ---- */
    CHECK(ppk((int32_t)measure_freq(440, 48000), 440) <= 10, "440 Hz within 1%");
    CHECK(ppk((int32_t)measure_freq(1000, 48000), 1000) <= 10, "1 kHz within 1%");

    /* ---- amplitude scaling ---- */
    sine_gen_t g;
    sine_gen_init(&g, 440, 48000);
    sine_gen_set_amplitude(&g, 16384);   /* half scale */
    int16_t peak = 0;
    for (uint32_t i = 0; i < 48000; i++) {
        int16_t s = sine_gen_next(&g);
        if (s > peak) peak = s;
    }
    CHECK(peak > 16000 && peak < 16500, "amplitude 16384 peaks near half scale");

    /* ---- click-free frequency change (the key acceptance) ----
     * Change 440 -> 880 Hz mid-stream; the sample-to-sample step at the
     * transition must be no larger than the normal maximum step of an 880 Hz
     * sine, i.e. the waveform stays continuous (no jump = no click).  The
     * theoretical max step at 880 Hz is ~ 2*pi*880/48000 * 32767 ~ 3771. */
    sine_gen_init(&g, 440, 48000);
    int16_t last = 0;
    for (int i = 0; i < 500; i++)
        last = sine_gen_next(&g);
    sine_gen_set_freq(&g, 880, 48000);        /* phase preserved */
    int16_t after = sine_gen_next(&g);
    int step = after - last;
    if (step < 0) step = -step;
    CHECK(step <= 4000, "440->880 Hz change is continuous (step within slope bound)");

    /* A phase *reset* on the same change would jump to table[0]=0 from a
     * non-zero sample, often a much larger step; confirm our step is small
     * relative to full scale (a real click would be tens of thousands). */
    CHECK(step < 32767 / 4, "frequency change introduces no large discontinuity");

    /* ---- stereo fill ---- */
    int16_t buf[8];
    sine_gen_init(&g, 440, 48000);
    sine_gen_fill(&g, buf, 4);
    CHECK(buf[0] == buf[1] && buf[2] == buf[3], "fill writes equal L/R (mono->stereo)");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
