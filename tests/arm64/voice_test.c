/* tests/arm64/voice_test.c - host unit tests for the synth voice architecture
 * (Theme M19, issue #189).
 *
 * The acceptance criteria, measured on rendered audio:
 *   - a note through the per-voice filter with a filter envelope shows the
 *     expected time-varying brightness (the spectral centroid, computed with
 *     the SDK's own rFFT, rises with the envelope and falls back);
 *   - unison produces multiple detuned partials around the fundamental
 *     (Goertzel probes at the exact expected sideband frequencies);
 *   - glide ramps pitch continuously between two notes (windowed
 *     zero-crossing frequency estimates walk monotonically from A to B);
 *   - MONO retriggers the envelope, LEGATO does not;
 *   - the mod matrix drives pitch (via tessera_synth_mod) end to end;
 *   - with every #189 feature off, the engine behaves like #113 (voice
 *     stealing and MPE tests still pass in their own suites).
 *
 * Build/run via:  make test-arm-voice
 */

#include "tessera.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define SR 48000.0f

static tessera_voice_t g_voices[8];

/* Goertzel power at f over x[0..n). */
static double goertzel(const float *x, uint32_t n, double f)
{
    double w = 2.0 * M_PI * f / (double)SR;
    double c = 2.0 * cos(w), s0, s1 = 0.0, s2 = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        s0 = (double)x[i] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* Spectral centroid (Hz) of a 2048-sample window, via the SDK rFFT.  The
 * frame is Hann-windowed and the weighting is POWER, not amplitude - a
 * rectangular window's leakage skirts would otherwise swamp the (heavily
 * attenuated) high end of a filtered signal and flatten the measurement. */
#define CN 2048u
static tessera_cpx_t g_ctw[CN / 2u];
static float g_cwin[CN];
static float centroid(const float *x)
{
    static float frame[CN];
    static tessera_cpx_t bins[CN / 2u + 1u];
    for (uint32_t i = 0; i < CN; i++)
        frame[i] = x[i] * g_cwin[i];
    tessera_rfft(frame, bins, g_ctw, CN);
    double num = 0.0, den = 0.0;
    for (uint32_t k = 1; k <= CN / 2u; k++) {
        double p = (double)bins[k].re * bins[k].re +
                   (double)bins[k].im * bins[k].im;
        double f = (double)k * SR / (double)CN;
        num += f * p;
        den += p;
    }
    return den > 0.0 ? (float)(num / den) : 0.0f;
}

/* Windowed zero-crossing frequency estimate. */
static float zc_freq(const float *x, uint32_t n)
{
    int up = 0;
    uint32_t first = 0, last = 0;
    for (uint32_t i = 1; i < n; i++) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            if (up == 0) first = i;
            last = i;
            up++;
        }
    }
    if (up < 2) return 0.0f;
    return (float)(up - 1) * SR / (float)(last - first);
}

/* ---- filter envelope: time-varying brightness ------------------------------ */

static void test_filter_envelope(void)
{
    printf("- filter envelope: brightness rises and falls with the sweep\n");
    tessera_synth_t s;
    tessera_synth_init(&s, g_voices, 8, SR);
    tessera_synth_set(&s, TESSERA_WAVE_SAW, 2.0f, 50.0f, 0.9f, 100.0f);
    /* Base cutoff 200 Hz; the filter env sweeps up to +6 kHz with a 300 ms
     * attack and a 400 ms decay back to a dark sustain, so each probe window
     * sits well inside one phase of the sweep. */
    tessera_synth_set_filter(&s, 1, 200.0f, 0.2f, 6000.0f, 0.0f,
                             300.0f, 400.0f, 0.02f, 100.0f);

    enum { NS = 57600 };                          /* 1.2 s */
    static float out[NS];
    tessera_synth_note_on(&s, 45, 127);           /* A2, 110 Hz */
    for (int i = 0; i < NS; i++)
        out[i] = tessera_synth_render(&s);

    float c_early = centroid(out + 960);                 /* 20..63 ms: env low */
    float c_peak  = centroid(out + 14400);               /* 300..343 ms: peak  */
    float c_late  = centroid(out + 52000);               /* 1083 ms: sustain   */
    printf("    centroid early=%.0f Hz peak=%.0f Hz late=%.0f Hz\n",
           (double)c_early, (double)c_peak, (double)c_late);
    CHECK(c_peak > 1.5f * c_early, "centroid rises with the filter attack");
    CHECK(c_peak > 1.5f * c_late, "centroid falls back with the filter decay");

    /* The same note with the filter off is static: peak ~= late. */
    tessera_synth_set_filter(&s, 0, 0, 0, 0, 0, 1, 1, 1, 1);
    tessera_synth_note_on(&s, 45, 127);
    for (int i = 0; i < NS; i++)
        out[i] = tessera_synth_render(&s);
    float o_peak = centroid(out + 14400), o_late = centroid(out + 52000);
    CHECK(fabsf(o_peak - o_late) < 0.15f * o_peak,
          "filter off: brightness is static");
}

static void test_key_tracking(void)
{
    printf("- key tracking: cutoff follows the note\n");
    tessera_synth_t s;
    tessera_synth_init(&s, g_voices, 8, SR);
    tessera_synth_set(&s, TESSERA_WAVE_SAW, 2.0f, 50.0f, 0.9f, 100.0f);
    /* Static dark filter (no envelope), full key tracking. */
    tessera_synth_set_filter(&s, 1, 400.0f, 0.0f, 0.0f, 1.0f,
                             1.0f, 1.0f, 1.0f, 1.0f);

    enum { NS = 8192 };
    static float lo[NS], hi[NS];
    tessera_synth_note_on(&s, 36, 127);          /* C2 */
    for (int i = 0; i < NS; i++) lo[i] = tessera_synth_render(&s);
    tessera_synth_note_off(&s, 36);
    for (int i = 0; i < 24000; i++) (void)tessera_synth_render(&s);

    tessera_synth_note_on(&s, 60, 127);          /* C4: two octaves up */
    for (int i = 0; i < NS; i++) hi[i] = tessera_synth_render(&s);

    /* Brightness relative to the fundamental: centroid / note_hz.  With full
     * tracking the ratio stays roughly constant; without tracking the high
     * note would be much duller relative to its fundamental. */
    float r_lo = centroid(lo + 2048) / tessera_note_to_hz(36);
    float r_hi = centroid(hi + 2048) / tessera_note_to_hz(60);
    printf("    centroid/f0: C2=%.2f C4=%.2f\n", (double)r_lo, (double)r_hi);
    CHECK(r_hi > 0.5f * r_lo && r_hi < 2.0f * r_lo,
          "relative brightness roughly constant across two octaves");
}

/* ---- unison ----------------------------------------------------------------- */

static void test_unison(void)
{
    printf("- unison: detuned partials appear around the fundamental\n");
    tessera_synth_t s;
    tessera_synth_init(&s, g_voices, 8, SR);
    tessera_synth_set(&s, TESSERA_WAVE_SAW, 1.0f, 10.0f, 1.0f, 50.0f);

    enum { NS = 65536 };
    static float out[NS];

    /* Three copies over 30 cents total width: sidebands at +/-15 cents. */
    tessera_synth_set_unison(&s, 3, 30.0f);
    tessera_synth_note_on(&s, 69, 127);
    for (int i = 0; i < NS; i++) out[i] = tessera_synth_render(&s);

    double f_lo = 440.0 * pow(2.0, -15.0 / 1200.0);   /* 436.2 Hz */
    double f_hi = 440.0 * pow(2.0,  15.0 / 1200.0);   /* 443.8 Hz */
    const float *ss = out + 16384;                    /* steady state */
    uint32_t sn = NS - 16384;
    double p_lo  = goertzel(ss, sn, f_lo);
    double p_mid = goertzel(ss, sn, 440.0);
    double p_hi  = goertzel(ss, sn, f_hi);
    double p_out = goertzel(ss, sn, 452.0);           /* outside the cluster */
    CHECK(p_lo > 20.0 * p_out && p_hi > 20.0 * p_out && p_mid > 20.0 * p_out,
          "all three detuned partials stand out");

    /* Without unison, the sidebands vanish. */
    tessera_synth_set_unison(&s, 1, 0.0f);
    tessera_synth_note_on(&s, 69, 127);
    for (int i = 0; i < NS; i++) out[i] = tessera_synth_render(&s);
    double q_lo  = goertzel(out + 16384, sn, f_lo);
    double q_mid = goertzel(out + 16384, sn, 440.0);
    CHECK(q_mid > 50.0 * q_lo, "single voice: no sideband partials");
}

/* ---- glide / portamento ------------------------------------------------------ */

static void test_glide(void)
{
    printf("- glide ramps pitch continuously between two notes\n");
    tessera_synth_t s;
    tessera_synth_init(&s, g_voices, 8, SR);
    tessera_synth_set(&s, TESSERA_WAVE_SINE, 1.0f, 10.0f, 1.0f, 50.0f);
    tessera_synth_set_mode(&s, TESSERA_SYNTH_MONO, 100.0f);   /* 100 ms glide */

    enum { NS = 24000 };                                       /* 500 ms */
    static float out[NS];
    tessera_synth_note_on(&s, 57, 127);                        /* A3, 220 Hz */
    for (int i = 0; i < 9600; i++) (void)tessera_synth_render(&s);
    tessera_synth_note_on(&s, 69, 127);                        /* glide to A4 */
    for (int i = 0; i < NS; i++) out[i] = tessera_synth_render(&s);

    float f1 = zc_freq(out + 960,  1440);     /* 20..50 ms into the glide  */
    float f2 = zc_freq(out + 2880, 1440);     /* 60..90 ms                 */
    float f3 = zc_freq(out + 7200, 4800);     /* 150..250 ms (settled)     */
    printf("    glide freq: %.1f -> %.1f -> %.1f Hz\n",
           (double)f1, (double)f2, (double)f3);
    CHECK(f1 > 230.0f && f1 < 330.0f, "early in the glide: between the notes");
    CHECK(f2 > f1, "pitch keeps rising through the glide");
    CHECK(f2 > 300.0f && f2 < 435.0f, "late in the glide: approaching the target");
    CHECK(fabsf(f3 - 440.0f) < 8.0f, "settles on the target note");
}

static void test_mono_vs_legato(void)
{
    printf("- MONO retriggers the envelope, LEGATO does not\n");
    tessera_synth_t s;
    tessera_synth_init(&s, g_voices, 8, SR);
    tessera_synth_set(&s, TESSERA_WAVE_SINE, 5.0f, 40.0f, 0.5f, 50.0f);

    /* MONO: the second note restarts the attack. */
    tessera_synth_set_mode(&s, TESSERA_SYNTH_MONO, 20.0f);
    tessera_synth_note_on(&s, 57, 127);
    for (int i = 0; i < 12000; i++) (void)tessera_synth_render(&s);   /* sustain */
    CHECK(g_voices[0].adsr.stage == TESSERA_ADSR_SUSTAIN, "settled in sustain");
    tessera_synth_note_on(&s, 69, 127);
    CHECK(g_voices[0].adsr.stage == TESSERA_ADSR_ATTACK,
          "MONO: overlapping note retriggers the attack");

    /* LEGATO: the second note glides without touching the envelope. */
    tessera_synth_init(&s, g_voices, 8, SR);
    tessera_synth_set(&s, TESSERA_WAVE_SINE, 5.0f, 40.0f, 0.5f, 50.0f);
    tessera_synth_set_mode(&s, TESSERA_SYNTH_LEGATO, 20.0f);
    tessera_synth_note_on(&s, 57, 127);
    for (int i = 0; i < 12000; i++) (void)tessera_synth_render(&s);
    tessera_synth_note_on(&s, 69, 127);
    CHECK(g_voices[0].adsr.stage == TESSERA_ADSR_SUSTAIN,
          "LEGATO: envelope stays in sustain across the overlap");
    CHECK(g_voices[0].glide_step > 0.0f, "LEGATO: pitch glides to the new note");
    CHECK(g_voices[0].note == 69, "the voice now belongs to the new note");

    /* A separated (non-overlapping) legato note still retriggers. */
    tessera_synth_note_off(&s, 69);
    for (int i = 0; i < 12000; i++) (void)tessera_synth_render(&s);   /* release out */
    tessera_synth_note_on(&s, 60, 127);
    CHECK(g_voices[0].adsr.stage == TESSERA_ADSR_ATTACK,
          "LEGATO: a detached note retriggers normally");
}

/* ---- mod-matrix integration --------------------------------------------------- */

static void test_mod_integration(void)
{
    printf("- the mod matrix drives synth pitch end to end\n");
    tessera_synth_t s;
    tessera_synth_init(&s, g_voices, 8, SR);
    tessera_synth_set(&s, TESSERA_WAVE_SINE, 1.0f, 10.0f, 1.0f, 50.0f);

    /* Matrix: one source routed to pitch with depth +12 semitones. */
    tessera_mod_route_t routes[4];
    float sources[2];
    tessera_mod_dest_t dests[2];
    tessera_mod_t m;
    tessera_mod_init(&m, routes, 4, sources, 2, dests, 2);
    tessera_mod_dest_setup(&m, 0, 0.0f, -24.0f, 24.0f);   /* pitch, semitones */
    tessera_mod_route(&m, 0, 0, 12.0f, TESSERA_MOD_LIN);

    enum { NS = 9600 };
    static float out[NS];
    tessera_synth_note_on(&s, 69, 127);                   /* 440 Hz */

    tessera_mod_set_source(&m, 0, 0.0f);                  /* block 1: no mod */
    tessera_mod_eval(&m);
    tessera_synth_mod(&s, tessera_mod_value(&m, 0), 0.0f, 1.0f);
    for (int i = 0; i < NS; i++) out[i] = tessera_synth_render(&s);
    float f0 = zc_freq(out + 2400, 4800);

    tessera_mod_set_source(&m, 0, 1.0f);                  /* block 2: full mod */
    tessera_mod_eval(&m);
    tessera_synth_mod(&s, tessera_mod_value(&m, 0), 0.0f, 1.0f);
    for (int i = 0; i < NS; i++) out[i] = tessera_synth_render(&s);
    float f1 = zc_freq(out + 2400, 4800);

    printf("    unmodulated %.1f Hz, modulated %.1f Hz\n", (double)f0, (double)f1);
    CHECK(fabsf(f0 - 440.0f) < 5.0f, "no modulation: the note itself");
    CHECK(fabsf(f1 - 880.0f) < 10.0f, "source 1.0 x depth 12 st: up an octave");

    /* Amp is a destination too. */
    tessera_synth_mod(&s, 0.0f, 0.0f, 0.0f);
    float silent = tessera_synth_render(&s);
    CHECK(silent == 0.0f, "amp modulation to zero silences the mix");
}

int main(void)
{
    printf("=== synth voice architecture host tests (issue #189) ===\n");
    tessera_fft_twiddles(g_ctw, CN);
    tessera_window_hann(g_cwin, CN);

    test_filter_envelope();
    test_key_tracking();
    test_unison();
    test_glide();
    test_mono_vs_legato();
    test_mod_integration();

    if (g_fail) {
        printf("VOICE TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("VOICE TESTS: ALL PASS\n");
    return 0;
}
