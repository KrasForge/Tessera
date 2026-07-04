/* tests/arm64/fx_test.c - host unit tests for the SDK reference effects suite
 * (Theme B, issue #111).
 *
 * Each effect is checked for its characteristic behaviour against a libm
 * reference tone: overdrive saturates and stays bounded, the compressor pulls a
 * loud signal down toward the threshold, the 3-band EQ boosts/cuts the right
 * bands, the delay reproduces its input after the set delay and decays with
 * feedback, the chorus stays bounded around unity, the gate mutes quiet signal
 * and passes loud, the reverb builds and then decays a tail, and the tuner
 * recovers the fundamental of a sine (with the right nearest note / cents).
 *
 * Build/run via:  make test-arm-fx
 */

#include "tessera.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define SR 48000.0f
#define PI_F 3.14159265358979323846f

static float rms(const float *x, int n)
{
    double acc = 0.0;
    for (int i = 0; i < n; i++) acc += (double)x[i] * x[i];
    return (float)sqrt(acc / n);
}

static void test_overdrive(void)
{
    printf("- overdrive: soft-clips, stays bounded, odd-symmetric\n");
    /* A hot input is pulled well below its linear value and never exceeds
     * `level`. */
    CHECK(tessera_fx_overdrive(0.0f, 4.0f, 1.0f) == 0.0f, "zero in -> zero out");
    float big = tessera_fx_overdrive(1.0f, 4.0f, 1.0f);
    CHECK(big > 0.7f && big <= 1.0f, "hard drive saturates at the ceiling");
    CHECK(tessera_fx_overdrive(-0.5f, 4.0f, 1.0f) ==
          -tessera_fx_overdrive(0.5f, 4.0f, 1.0f), "odd symmetry");
    /* Small signals pass through with roughly `drive*level` slope. */
    float small = tessera_fx_overdrive(0.01f, 2.0f, 1.0f);
    CHECK(small > 0.015f && small < 0.021f, "near-linear for small signals");
    /* level scales the output. */
    CHECK(fabsf(tessera_fx_overdrive(1.0f, 4.0f, 0.5f) - 0.5f * big) < 1e-6f,
          "level scales the result");
}

static void test_compressor(void)
{
    printf("- compressor: gain reduction above threshold, unity below\n");
    /* 4:1 above -20 dB (0.1 linear), fast attack. */
    tessera_fx_comp_t c;
    tessera_fx_comp_init(&c, SR, 1.0f, 50.0f, -20.0f, 4.0f, 0.0f);

    /* A quiet -40 dB tone (amp 0.01) passes essentially untouched. */
    float quiet_in = 0.01f, quiet_out = 0.0f;
    for (int i = 0; i < 4800; i++) {
        float x = quiet_in * sinf(2.0f * PI_F * 220.0f * (float)i / SR);
        float y = tessera_fx_comp(&c, x);
        if (i >= 4700) quiet_out = fabsf(y) > quiet_out ? fabsf(y) : quiet_out;
    }
    CHECK(quiet_out > 0.008f, "below threshold: near unity gain");

    /* A loud 0 dB tone (amp 1.0) is compressed: its steady-state peak is pulled
     * far below the input peak. */
    tessera_fx_comp_init(&c, SR, 1.0f, 50.0f, -20.0f, 4.0f, 0.0f);
    float loud_peak = 0.0f;
    for (int i = 0; i < 9600; i++) {
        float x = 1.0f * sinf(2.0f * PI_F * 220.0f * (float)i / SR);
        float y = tessera_fx_comp(&c, x);
        if (i >= 9500) loud_peak = fabsf(y) > loud_peak ? fabsf(y) : loud_peak;
    }
    CHECK(loud_peak < 0.5f, "above threshold: loud signal is pulled down");
    CHECK(loud_peak > 0.1f,  "but not below the threshold");
}

static void test_eq3(void)
{
    printf("- 3-band EQ: boost low, cut mid, boost high\n");
    /* +12 dB low shelf @ 120, -12 dB peak @ 1k, +6 dB high shelf @ 6k. */
    float low = 0, mid = 0, high = 0;
    const float amp = 0.25f;
    struct { float f; float *slot; } bands[3] = {
        { 80.0f, &low }, { 1000.0f, &mid }, { 9000.0f, &high },
    };
    for (int b = 0; b < 3; b++) {
        tessera_fx_eq3_t eq;
        tessera_fx_eq3_init(&eq, SR, 120.0f, 12.0f, 1000.0f, 1.0f, -12.0f, 6000.0f, 6.0f);
        double in = 0, out = 0; int n = 0;
        for (int i = 0; i < 9600; i++) {
            float x = amp * sinf(2.0f * PI_F * bands[b].f * (float)i / SR);
            float y = tessera_fx_eq3(&eq, x);
            if (i >= 4800) { in += (double)x * x; out += (double)y * y; n++; }
        }
        *bands[b].slot = (float)sqrt(out / in);       /* gain ratio */
    }
    CHECK(low  > 1.5f, "low band boosted (> +3 dB)");
    CHECK(mid  < 0.5f, "mid band cut (< -6 dB)");
    CHECK(high > 1.4f, "high band boosted (> +3 dB)");
}

static void test_delay(void)
{
    printf("- delay: an impulse reappears after the set delay and decays\n");
    static float buf[2048];
    tessera_fx_delay_t d;
    tessera_fx_delay_init(&d, buf, 2048);
    tessera_fx_delay_set(&d, 100.0f, 0.5f, 1.0f);      /* 100-sample, 0.5 fb, full wet */

    /* Read-before-write gives the delay one sample of latency, and each
     * feedback pass one more, so scan a small window around each expected tap
     * rather than pinning an exact index. */
    float y[400];
    int   i1 = 0, i2 = 0;
    for (int i = 0; i < 400; i++) {
        float x = (i == 0) ? 1.0f : 0.0f;
        y[i] = tessera_fx_delay(&d, x);
    }
    for (int i = 90;  i < 115; i++) if (y[i] > y[i1]) i1 = i;
    for (int i = 190; i < 220; i++) if (y[i] > y[i2]) i2 = i;
    CHECK(i1 >= 100 && i1 <= 102, "first echo lands ~100 samples out");
    CHECK(y[i1] > 0.9f, "first echo ~= full amplitude");
    CHECK(y[i2] > 0.4f && y[i2] < 0.6f, "second echo ~= feedback^1 (0.5)");
}

static void test_chorus(void)
{
    printf("- chorus: stays bounded and adds motion around the dry signal\n");
    static float buf[4096];
    tessera_fx_chorus_t c;
    tessera_fx_chorus_init(&c, buf, 4096, SR, 1.0f, 20.0f, 5.0f, 0.5f);
    float out[9600];
    for (int i = 0; i < 9600; i++) {
        float x = 0.5f * sinf(2.0f * PI_F * 300.0f * (float)i / SR);
        out[i] = tessera_fx_chorus(&c, x);
    }
    float peak = 0;
    for (int i = 0; i < 9600; i++) if (fabsf(out[i]) > peak) peak = fabsf(out[i]);
    CHECK(peak < 1.0f, "output stays bounded");
    CHECK(rms(out + 4800, 4800) > 0.1f, "signal is present after the delay fills");
}

static void test_gate(void)
{
    printf("- noise gate: mutes below threshold, passes above\n");
    tessera_fx_gate_t g;
    tessera_fx_gate_init(&g, SR, -30.0f, 1.0f, 5.0f);   /* thresh ~0.0316 */

    /* Quiet -50 dB signal (amp 0.003) is muted. */
    float quiet_peak = 0;
    for (int i = 0; i < 4800; i++) {
        float x = 0.003f * sinf(2.0f * PI_F * 200.0f * (float)i / SR);
        float y = tessera_fx_gate(&g, x);
        if (i >= 4700) quiet_peak = fabsf(y) > quiet_peak ? fabsf(y) : quiet_peak;
    }
    CHECK(quiet_peak < 0.001f, "quiet signal is gated (near silent)");

    /* Loud -6 dB signal (amp 0.5) passes. */
    tessera_fx_gate_init(&g, SR, -30.0f, 1.0f, 5.0f);
    float loud_peak = 0;
    for (int i = 0; i < 4800; i++) {
        float x = 0.5f * sinf(2.0f * PI_F * 200.0f * (float)i / SR);
        float y = tessera_fx_gate(&g, x);
        if (i >= 4700) loud_peak = fabsf(y) > loud_peak ? fabsf(y) : loud_peak;
    }
    CHECK(loud_peak > 0.4f, "loud signal passes the gate");
}

static void test_reverb(void)
{
    printf("- reverb: builds a decaying tail after an impulse\n");
    static float c0[1557], c1[1617], c2[1491], c3[1422], a0[225], a1[556];
    float *cbuf[4] = { c0, c1, c2, c3 };
    uint32_t csz[4] = { 1557, 1617, 1491, 1422 };
    float *abuf[2] = { a0, a1 };
    uint32_t asz[2] = { 225, 556 };
    tessera_fx_reverb_t r;
    tessera_fx_reverb_init(&r, cbuf, csz, abuf, asz, 0.84f, 0.2f, 1.0f);

    float early = 0, mid = 0, late = 0;
    for (int i = 0; i < 48000; i++) {
        float x = (i == 0) ? 1.0f : 0.0f;
        float y = tessera_fx_reverb(&r, x);
        float a = fabsf(y);
        if (i < 4000)                 early += a;
        else if (i < 12000)           mid   += a;
        else if (i < 40000)           late  += a;
    }
    CHECK(mid > 0.0f, "a tail exists well after the impulse");
    /* Energy decays: normalise per-sample and confirm late < mid. */
    CHECK(late / 28000.0f < mid / 8000.0f, "the tail decays over time");
}

static void test_tuner(void)
{
    printf("- tuner: recovers the fundamental and nearest note\n");
    tessera_fx_tuner_t t;
    tessera_fx_tuner_init(&t, SR);
    static float tone[4096];
    for (int i = 0; i < 4096; i++)
        tone[i] = sinf(2.0f * PI_F * 440.0f * (float)i / SR);
    tessera_fx_tuner_process(&t, tone, 4096);
    float hz = tessera_fx_tuner_hz(&t);
    CHECK(fabsf(hz - 440.0f) < 1.0f, "A4 sine estimated at ~440 Hz");

    float cents;
    int note = tessera_fx_note_of(hz, &cents);
    CHECK(note == 69, "nearest note is MIDI 69 (A4)");
    CHECK(fabsf(cents) < 5.0f, "within a few cents of in-tune");

    /* A slightly sharp 452 Hz reads as A4 but sharp (positive cents). */
    int sharp = tessera_fx_note_of(452.0f, &cents);
    CHECK(sharp == 69 && cents > 20.0f, "452 Hz is A4, sharp by tens of cents");

    /* Silence yields no estimate. */
    static float quiet[1024] = { 0 };
    tessera_fx_tuner_process(&t, quiet, 1024);
    CHECK(tessera_fx_tuner_hz(&t) == 0.0f, "silence -> no pitch");
}

int main(void)
{
    printf("=== Tessera SDK effects-suite tests (Theme B, #111) ===\n");
    test_overdrive();
    test_compressor();
    test_eq3();
    test_delay();
    test_chorus();
    test_gate();
    test_reverb();
    test_tuner();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
