/* tests/arm64/synth_test.c - host unit tests for the SDK polyphonic synth
 * engine (Theme B, issue #113).
 *
 * Proves the note-event -> audio path: a chord allocates one voice per note and
 * produces sound, a note-off releases and eventually frees its voice, the engine
 * plays the right pitch for a note, ABI note events drive it, and polyphony is
 * bounded by voice stealing under over-subscription.
 *
 * Build/run via:  make test-arm-synth
 */

#include "tessera.h"

#include <stdio.h>
#include <stdint.h>
#include <math.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define SR 48000.0f
#define PI_F 3.14159265358979323846

/* Render `n` samples into `out`, returning peak absolute amplitude. */
static float render(tessera_synth_t *s, float *out, int n)
{
    float peak = 0.0f;
    for (int i = 0; i < n; i++) {
        out[i] = tessera_synth_render(s);
        float a = fabsf(out[i]);
        if (a > peak) peak = a;
    }
    return peak;
}

/* Dominant frequency of a buffer via interpolated upward zero-crossings. */
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

static void test_note_to_hz(void)
{
    printf("- MIDI note -> frequency\n");
    CHECK(fabsf(tessera_note_to_hz(69) - 440.0f) < 0.1f, "A4 (69) = 440 Hz");
    CHECK(fabsf(tessera_note_to_hz(81) - 880.0f) < 0.5f, "A5 (81) = 880 Hz (octave up)");
    CHECK(fabsf(tessera_note_to_hz(60) - 261.63f) < 0.5f, "middle C (60) ~= 261.6 Hz");
}

static void test_chord_polyphony(void)
{
    printf("- a chord allocates one voice per note and sounds\n");
    tessera_voice_t voices[8];
    tessera_synth_t s;
    tessera_synth_init(&s, voices, 8, SR);

    tessera_synth_note_on(&s, 60, 100);
    tessera_synth_note_on(&s, 64, 100);
    tessera_synth_note_on(&s, 67, 100);
    CHECK(tessera_synth_active(&s) == 3, "three notes -> three active voices");

    static float buf[4800];
    float peak = render(&s, buf, 4800);
    CHECK(peak > 0.1f, "the chord produces audio");
}

static void test_pitch(void)
{
    printf("- a single note plays at the right pitch\n");
    tessera_voice_t voices[4];
    tessera_synth_t s;
    tessera_synth_init(&s, voices, 4, SR);
    /* Long sustain so the tone is steady across the analysis window. */
    tessera_synth_set(&s, TESSERA_WAVE_SINE, 1.0f, 10.0f, 1.0f, 200.0f);
    tessera_synth_note_on(&s, 69, 127);          /* A4 */

    static float buf[8192];
    render(&s, buf, 8192);
    float hz = est_hz(buf + 1024, 7168);         /* skip the attack transient */
    CHECK(fabsf(hz - 440.0f) < 3.0f, "A4 renders at ~440 Hz");
}

static void test_note_off_frees_voice(void)
{
    printf("- note-off releases the voice and it is eventually reclaimed\n");
    tessera_voice_t voices[4];
    tessera_synth_t s;
    tessera_synth_init(&s, voices, 4, SR);
    tessera_synth_set(&s, TESSERA_WAVE_SINE, 1.0f, 10.0f, 0.8f, 20.0f);  /* 20 ms release */
    tessera_synth_note_on(&s, 72, 100);

    static float buf[512];
    render(&s, buf, 512);
    CHECK(tessera_synth_active(&s) == 1, "voice active while held");

    tessera_synth_note_off(&s, 72);
    /* Render past the 20 ms release (~960 samples) with margin. */
    static float tail[4800];
    render(&s, tail, 4800);
    CHECK(tessera_synth_active(&s) == 0, "voice freed after release completes");
}

static void test_event_drive(void)
{
    printf("- ABI note events drive the engine\n");
    tessera_voice_t voices[4];
    tessera_synth_t s;
    tessera_synth_init(&s, voices, 4, SR);
    tessera_synth_set(&s, TESSERA_WAVE_SAW, 1.0f, 10.0f, 0.8f, 20.0f);  /* 20 ms release */

    tessera_note_event_t on  = { TESSERA_EV_NOTE_ON,  0, 65, 90 };
    tessera_note_event_t off = { TESSERA_EV_NOTE_OFF, 0, 65, 0  };
    tessera_synth_event(&s, &on);
    CHECK(tessera_synth_active(&s) == 1, "NOTE_ON event starts a voice");
    static float buf[256];
    render(&s, buf, 256);
    tessera_synth_event(&s, &off);
    static float tail[4800];
    render(&s, tail, 4800);
    CHECK(tessera_synth_active(&s) == 0, "NOTE_OFF event releases it");

    /* A CC event is ignored by the engine. */
    tessera_note_event_t cc = { TESSERA_EV_CC, 0, 7, 100 };
    tessera_synth_event(&s, &cc);
    CHECK(tessera_synth_active(&s) == 0, "a CC event allocates no voice");
}

static void test_voice_stealing(void)
{
    printf("- polyphony is bounded: extra notes steal, never overflow\n");
    tessera_voice_t voices[4];
    tessera_synth_t s;
    tessera_synth_init(&s, voices, 4, SR);
    for (int note = 60; note < 60 + 8; note++)   /* 8 notes into 4 voices */
        tessera_synth_note_on(&s, note, 100);
    CHECK(tessera_synth_active(&s) <= 4, "never more than n_voices sounding");
    CHECK(tessera_synth_active(&s) == 4, "all voices in use after over-subscription");
    static float buf[512];
    CHECK(render(&s, buf, 512) > 0.1f, "still producing audio after stealing");
}

static void test_fm_waveform(void)
{
    printf("- FM waveform: an FM voice sounds and adds upper harmonics (#164)\n");
    tessera_voice_t voices[4];
    tessera_synth_t s;
    tessera_synth_init(&s, voices, 4, SR);
    tessera_synth_set(&s, TESSERA_WAVE_FM, 1.0f, 10.0f, 1.0f, 200.0f);
    tessera_synth_set_fm(&s, 1.0f, 1.5f);        /* ratio 1, index 1.5 */
    tessera_synth_note_on(&s, 69, 127);          /* A4 = 440 Hz */

    static float buf[8192];
    render(&s, buf, 8192);
    /* The FM voice produces audio and, at index 1.5, clear energy at 2*f0 that a
     * pure sine would not have. */
    float peak = 0;
    for (int i = 0; i < 8192; i++) if (fabsf(buf[i]) > peak) peak = fabsf(buf[i]);
    CHECK(peak > 0.1f, "FM voice produces audio");

    /* Crude 2nd-harmonic probe via Goertzel at 880 Hz. */
    double w = 2.0 * PI_F * 880.0 / SR, c = 2.0 * cos(w), s1 = 0, s2 = 0;
    for (int i = 1024; i < 8192; i++) { double s0 = buf[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    double mag = sqrt((s1 - s2 * cos(w)) * (s1 - s2 * cos(w)) + (s2 * sin(w)) * (s2 * sin(w)));
    CHECK(mag > 1.0, "FM adds a 2nd-harmonic component");
}

int main(void)
{
    printf("=== Tessera SDK polyphonic-synth tests (Theme B, #113) ===\n");
    test_note_to_hz();
    test_chord_polyphony();
    test_pitch();
    test_note_off_frees_voice();
    test_event_drive();
    test_voice_stealing();
    test_fm_waveform();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
