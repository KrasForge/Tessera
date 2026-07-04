/* tests/arm64/dsp_test.c - host unit tests for the SDK DSP building blocks
 * (Theme B).
 *
 * Each primitive is checked for its characteristic behaviour against a libm
 * reference: the smoother converges to its target, the biquads pass/reject the
 * right bands, the oscillator runs at the set frequency, the delay line
 * reproduces its input after the set delay (and interpolates fractionally), the
 * envelope follower tracks level with the right attack/release, and the ADSR
 * walks attack -> decay -> sustain -> release.
 *
 * Build/run via:  make test-arm-dsp
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

/* Steady-state RMS of a sine of frequency `f` pushed through a biquad. */
static float biquad_rms(tessera_biquad_t *bq, float f)
{
    tessera_biquad_reset(bq);
    double acc = 0.0; int n = 0;
    for (int i = 0; i < 4800; i++) {
        float x = sinf(2.0f * PI_F * f * (float)i / SR);
        float y = tessera_biquad_process(bq, x);
        if (i >= 2400) { acc += (double)y * y; n++; }   /* skip transient */
    }
    return (float)sqrt(acc / n);
}

static void test_smoother(void)
{
    printf("- one-pole smoother converges to its target, monotonically\n");
    tessera_smooth_t s; tessera_smooth_init(&s, SR, 10.0f);  /* 10 ms */
    tessera_smooth_set(&s, 0.0f);
    float prev = 0.0f; int mono = 1;
    for (int i = 0; i < 4800; i++) {
        float y = tessera_smooth(&s, 1.0f);
        if (y < prev - 1e-6f) mono = 0;
        prev = y;
    }
    CHECK(mono, "monotonically approaches the target");
    CHECK(fabsf(prev - 1.0f) < 0.01f, "reaches the target within 1%");
    /* after ~one time constant (10 ms) it should be well on its way */
    tessera_smooth_init(&s, SR, 10.0f); tessera_smooth_set(&s, 0.0f);
    for (int i = 0; i < 480; i++) tessera_smooth(&s, 1.0f);   /* 10 ms */
    CHECK(s.y > 0.5f && s.y < 0.75f, "at one time constant it is ~63% of the way");
}

static void test_biquads(void)
{
    printf("- biquads pass and reject the expected bands\n");
    tessera_biquad_t bq;

    tessera_biquad_lowpass(&bq, SR, 1000.0f, 0.707f);
    float lp_low = biquad_rms(&bq, 100.0f), lp_high = biquad_rms(&bq, 15000.0f);
    CHECK(lp_low > 0.6f, "low-pass passes 100 Hz (RMS ~0.7)");
    CHECK(lp_high < 0.05f, "low-pass rejects 15 kHz");

    tessera_biquad_highpass(&bq, SR, 1000.0f, 0.707f);
    float hp_low = biquad_rms(&bq, 100.0f), hp_high = biquad_rms(&bq, 15000.0f);
    CHECK(hp_high > 0.6f, "high-pass passes 15 kHz");
    CHECK(hp_low < 0.05f, "high-pass rejects 100 Hz");

    tessera_biquad_bandpass(&bq, SR, 1000.0f, 2.0f);
    float bp_c = biquad_rms(&bq, 1000.0f), bp_lo = biquad_rms(&bq, 100.0f),
          bp_hi = biquad_rms(&bq, 12000.0f);
    CHECK(bp_c > bp_lo * 3.0f && bp_c > bp_hi * 3.0f, "band-pass peaks near its centre");

    /* DC gain of a low-pass is ~unity */
    tessera_biquad_lowpass(&bq, SR, 1000.0f, 0.707f);
    float dc = 0.0f;
    for (int i = 0; i < 4800; i++) dc = tessera_biquad_process(&bq, 1.0f);
    CHECK(fabsf(dc - 1.0f) < 0.01f, "low-pass DC gain is unity");

    /* peaking EQ boosts at its centre */
    tessera_biquad_peaking(&bq, SR, 1000.0f, 1.0f, 12.0f);
    float pk = biquad_rms(&bq, 1000.0f);
    CHECK(pk > 2.0f, "peaking EQ (+12 dB) boosts its centre (~4x amplitude)");
}

static void test_oscillator(void)
{
    printf("- oscillator runs at the set frequency; waveforms in range\n");
    tessera_osc_t o = {0.0f, 0.0f};
    tessera_osc_set(&o, SR, 1000.0f);
    int crossings = 0; float prev = 0.0f; int N = 4800;   /* 100 ms -> 100 cycles */
    for (int i = 0; i < N; i++) {
        float y = tessera_osc_sin(&o);
        if (prev < 0.0f && y >= 0.0f) crossings++;
        prev = y;
    }
    float freq = (float)crossings * SR / (float)N;
    CHECK(fabsf(freq - 1000.0f) < 20.0f, "sine frequency within 20 Hz of 1000 Hz");

    tessera_osc_t s = {0.0f, 0.0f}; tessera_osc_set(&s, SR, 440.0f);
    float mn = 1e9f, mx = -1e9f, sum = 0.0f;
    for (int i = 0; i < 4800; i++) { float y = tessera_osc_saw(&s); if (y<mn)mn=y; if(y>mx)mx=y; sum+=y; }
    CHECK(mn > -1.2f && mx < 1.2f, "saw stays within [-1.2, 1.2]");
    CHECK(fabsf(sum / 4800.0f) < 0.05f, "saw is ~zero-mean");
}

static void test_delay(void)
{
    printf("- delay line reproduces its input after the set delay\n");
    float buf[64]; tessera_delay_t d; tessera_delay_init(&d, buf, 64);
    float out[40]; float peak = -1.0f; int peak_at = -1;
    for (int i = 0; i < 40; i++) {
        float x = (i == 0) ? 1.0f : 0.0f;          /* impulse at t=0 */
        out[i] = tessera_delay_tick(&d, x, 8.0f);   /* 8-sample delay */
        if (out[i] > peak) { peak = out[i]; peak_at = i; }
    }
    CHECK(peak_at == 8 && peak > 0.99f, "impulse reappears at exactly 8 samples");

    /* fractional read: a 0.5-sample delay of a ramp interpolates linearly */
    float b2[16]; tessera_delay_t f; tessera_delay_init(&f, b2, 16);
    tessera_delay_write(&f, 10.0f);
    tessera_delay_write(&f, 20.0f);                 /* most recent = 20 */
    float half = tessera_delay_read(&f, 0.5f);      /* between 20 and 10 */
    CHECK(fabsf(half - 15.0f) < 0.001f, "0.5-sample fractional read interpolates to 15");
}

static void test_envfollow(void)
{
    printf("- envelope follower tracks level with attack/release\n");
    tessera_envfollow_t e; tessera_envfollow_init(&e, SR, 1.0f, 50.0f);
    for (int i = 0; i < 4800; i++) tessera_envfollow(&e, 1.0f);   /* loud */
    float loud = e.env;
    CHECK(loud > 0.9f, "rises toward the input level under a loud signal");
    for (int i = 0; i < 240; i++) tessera_envfollow(&e, 0.0f);    /* 5 ms silence */
    CHECK(e.env < loud, "releases (decays) once the signal stops");
}

static void test_adsr(void)
{
    printf("- ADSR walks attack -> decay -> sustain -> release\n");
    tessera_adsr_t a; tessera_adsr_init(&a, SR, 5.0f, 5.0f, 0.5f, 10.0f);
    tessera_adsr_gate(&a, 1);
    float peak = 0.0f;
    for (int i = 0; i < 480; i++) { float v = tessera_adsr(&a); if (v > peak) peak = v; }
    CHECK(peak > 0.99f, "attack reaches full level");
    CHECK(fabsf(a.level - 0.5f) < 0.01f, "decays to the sustain level (0.5)");
    tessera_adsr_gate(&a, 0);
    for (int i = 0; i < 960; i++) tessera_adsr(&a);
    CHECK(a.level < 0.01f, "release falls back to zero");
}

int main(void)
{
    printf("=== Tessera SDK DSP building-block tests (Theme B) ===\n");
    test_smoother();
    test_biquads();
    test_oscillator();
    test_delay();
    test_envfollow();
    test_adsr();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
