/* tests/arm64/mpe_test.c - host unit tests for MPE / per-note expression
 * (Theme M17, issue #171): the decoder and its effect on the synth.
 *
 * Build/run via:  make test-arm-mpe
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

static float est_hz(const float *x, int n)
{
    float first = -1, last = -1; int count = 0;
    for (int i = 1; i < n; i++)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            float d = x[i] - x[i - 1];
            float pos = (float)(i - 1) + (d != 0.0f ? -x[i - 1] / d : 0.0f);
            if (first < 0) first = pos;
            last = pos; count++;
        }
    return (count >= 2 && last > first) ? (float)(count - 1) * SR / (last - first) : 0.0f;
}

static void test_decoder(void)
{
    printf("- MPE decoder tags channel expression onto the channel's note\n");
    tessera_mpe_t m; tessera_mpe_init(&m);
    tessera_note_event_t ev[4];
    int n;

    n = tessera_mpe_feed(&m, 0x91, 60, 100, ev, 4);   /* note-on ch1 C4 */
    CHECK(n == 1 && ev[0].type == TESSERA_EV_NOTE_ON && ev[0].data1 == 60, "note-on decoded");

    /* Pitch bend on ch1 -> PITCH for note 60, centred value 0 at 8192. */
    n = tessera_mpe_feed(&m, 0xE1, 0x00, 0x60, ev, 4);   /* 14-bit = 0x60<<7 = 12288 */
    CHECK(n == 1 && ev[0].type == TESSERA_EV_PITCH && ev[0].data1 == 60 &&
          ev[0].value == 12288 - 8192, "pitch bend tagged to the active note");

    /* A second note on ch2 gets its own independent expression. */
    tessera_mpe_feed(&m, 0x92, 64, 100, ev, 4);          /* note-on ch2 E4 */
    n = tessera_mpe_feed(&m, 0xE2, 0x00, 0x20, ev, 4);   /* bend ch2 */
    CHECK(n == 1 && ev[0].data1 == 64 && ev[0].value == (0x20 << 7) - 8192,
          "ch2 bend tags note 64, independent of ch1");

    /* Channel pressure and CC74 map to per-note PRESSURE / TIMBRE. */
    n = tessera_mpe_feed(&m, 0xD1, 90, 0, ev, 4);
    CHECK(n == 1 && ev[0].type == TESSERA_EV_PRESSURE && ev[0].data1 == 60 && ev[0].data2 == 90,
          "channel pressure -> per-note PRESSURE");
    n = tessera_mpe_feed(&m, 0xB1, 74, 50, ev, 4);
    CHECK(n == 1 && ev[0].type == TESSERA_EV_TIMBRE && ev[0].data1 == 60 && ev[0].data2 == 50,
          "CC74 -> per-note TIMBRE");

    /* An ordinary CC passes through; a bend with no active note is dropped. */
    n = tessera_mpe_feed(&m, 0xB1, 7, 100, ev, 4);
    CHECK(n == 1 && ev[0].type == TESSERA_EV_CC && ev[0].data1 == 7, "CC7 passes through");
    tessera_mpe_feed(&m, 0x81, 60, 0, ev, 4);            /* note-off ch1 */
    n = tessera_mpe_feed(&m, 0xE1, 0, 0x40, ev, 4);      /* bend, no active note */
    CHECK(n == 0, "pitch bend with no held note produces nothing");
}

static void test_synth_pitch_bend(void)
{
    printf("- a per-note PITCH event bends the synth voice's pitch\n");
    tessera_voice_t voices[4];
    tessera_synth_t s;
    tessera_synth_init(&s, voices, 4, SR);
    tessera_synth_set(&s, TESSERA_WAVE_SINE, 1.0f, 10.0f, 1.0f, 500.0f);
    tessera_synth_set_bend_range(&s, 48.0f);
    tessera_synth_note_on(&s, 69, 127);          /* A4 = 440 Hz */

    static float buf[8192];
    for (int i = 0; i < 8192; i++) buf[i] = tessera_synth_render(&s);
    CHECK(fabsf(est_hz(buf + 1024, 7168) - 440.0f) < 3.0f, "un-bent note plays at 440 Hz");

    /* Bend up exactly +12 semitones: value = 12/48 * 8192 = 2048 -> ~880 Hz. */
    tessera_note_event_t pitch = { TESSERA_EV_PITCH, 0, 69, 0, 2048, 0 };
    tessera_synth_event(&s, &pitch);
    for (int i = 0; i < 8192; i++) buf[i] = tessera_synth_render(&s);
    CHECK(fabsf(est_hz(buf + 1024, 7168) - 880.0f) < 6.0f, "+12 semitone bend -> ~880 Hz");
}

static void test_synth_pressure(void)
{
    printf("- a per-note PRESSURE event scales the voice level\n");
    tessera_voice_t voices[4];
    tessera_synth_t s;
    tessera_synth_init(&s, voices, 4, SR);
    tessera_synth_set(&s, TESSERA_WAVE_SINE, 1.0f, 10.0f, 1.0f, 500.0f);
    tessera_synth_note_on(&s, 60, 127);

    static float buf[4096];
    for (int i = 0; i < 4096; i++) buf[i] = tessera_synth_render(&s);
    float full = 0; for (int i = 2048; i < 4096; i++) if (fabsf(buf[i]) > full) full = fabsf(buf[i]);

    tessera_note_event_t pr = { TESSERA_EV_PRESSURE, 0, 60, 64, 0, 0 };  /* ~half */
    tessera_synth_event(&s, &pr);
    for (int i = 0; i < 4096; i++) buf[i] = tessera_synth_render(&s);
    float half = 0; for (int i = 2048; i < 4096; i++) if (fabsf(buf[i]) > half) half = fabsf(buf[i]);

    CHECK(half < full * 0.6f && half > full * 0.4f, "pressure 64/127 roughly halves the level");
}

int main(void)
{
    printf("=== Tessera MPE / per-note expression tests (M17, #171) ===\n");
    test_decoder();
    test_synth_pitch_bend();
    test_synth_pressure();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
