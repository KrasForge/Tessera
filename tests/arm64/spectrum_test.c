/* tests/arm64/spectrum_test.c - host unit tests for spectrum analysis, the
 * FFT tuner, and their OLED screens (Theme M18, issue #187).
 *
 * Covers the whole pipeline: the SDK analyser must put the energy of known
 * one- and two-tone inputs in the correct log-frequency bars (with peak-hold
 * decaying after the signal stops); the FFT tuner must estimate 440 Hz and a
 * detuned tone to within a cent or two and stay accurate under broadband
 * noise that derails the zero-crossing tuner; and the OLED UI model must
 * render both as new screens reachable from HOME.
 *
 * Build/run via:  make test-arm-spectrum
 */

#include "tessera.h"
#include "oled_ui.h"

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

#define N      4096u
#define SR     48000.0f
#define NBARS  16u
#define BINS   (N / 2u + 1u)

static tessera_cpx_t g_tw[N / 2u];
static float         g_fmem[3u * N + BINS];       /* ftuner (largest arena) */
static float         g_smem[2u * N];
static tessera_cpx_t g_cmem[BINS];
static uint32_t      g_umem[3u * NBARS + 1u];

static uint32_t g_seed = 0x5eed5eedu;
static float frand(void)
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return (float)(int32_t)g_seed / 2147483648.0f;
}

/* Which bar holds frequency f, per the analyser's own edges. */
static int bar_of(const tessera_spectrum_t *sp, double f)
{
    uint32_t bin = (uint32_t)(f * N / SR + 0.5);
    for (uint32_t b = 0; b < sp->nbars; b++)
        if (bin >= sp->edges[b] && bin < sp->edges[b + 1u])
            return (int)b;
    return -1;
}

/* ---- spectrum bars --------------------------------------------------------- */

static void test_spectrum_tones(void)
{
    printf("- known tones land in the correct log-frequency bars\n");
    tessera_spectrum_t sp;
    CHECK(tessera_spectrum_init(&sp, N, SR, NBARS, 50u, g_tw,
                                g_smem, g_cmem, g_umem) == 0, "init ok");

    static float x[N];
    for (uint32_t i = 0; i < N; i++)
        x[i] = (float)sin(2.0 * M_PI * 1000.0 * i / (double)SR);
    tessera_spectrum_process(&sp, x);

    int b1k = bar_of(&sp, 1000.0);
    CHECK(b1k >= 0, "1 kHz maps to a bar");
    uint32_t max = 0; int bmax = -1;
    for (uint32_t b = 0; b < NBARS; b++)
        if (sp.bars[b] > max) { max = sp.bars[b]; bmax = (int)b; }
    CHECK(bmax == b1k, "the 1 kHz bar is the loudest");
    CHECK(max > 900u, "a full-scale tone reads near 0 dBFS (>900 per-mille)");

    /* Every bar not adjacent to the tone's stays far below it. */
    int clean = 1;
    for (int b = 0; b < (int)NBARS; b++)
        if (b < b1k - 1 || b > b1k + 1)
            if (sp.bars[b] > max / 2u) clean = 0;
    CHECK(clean, "non-adjacent bars stay well below the tone");

    /* Two tones, far apart: both bars up. */
    for (uint32_t i = 0; i < N; i++)
        x[i] = (float)(0.5 * sin(2.0 * M_PI * 200.0 * i / (double)SR) +
                       0.5 * sin(2.0 * M_PI * 5000.0 * i / (double)SR));
    tessera_spectrum_process(&sp, x);
    int b200 = bar_of(&sp, 200.0), b5k = bar_of(&sp, 5000.0);
    CHECK(b200 != b5k && b200 >= 0 && b5k >= 0, "the two tones map to distinct bars");
    CHECK(sp.bars[b200] > 800u && sp.bars[b5k] > 800u,
          "both tone bars read loud");
}

static void test_peak_hold(void)
{
    printf("- peak-hold: bars drop immediately, peaks decay\n");
    tessera_spectrum_t sp;
    tessera_spectrum_init(&sp, N, SR, NBARS, 100u, g_tw, g_smem, g_cmem, g_umem);

    static float x[N], silence[N];
    for (uint32_t i = 0; i < N; i++) {
        x[i] = (float)sin(2.0 * M_PI * 1000.0 * i / (double)SR);
        silence[i] = 0.0f;
    }
    tessera_spectrum_process(&sp, x);
    int b = bar_of(&sp, 1000.0);
    uint32_t held = sp.peaks[b];
    CHECK(held > 900u, "peak captured at the tone level");

    tessera_spectrum_process(&sp, silence);
    CHECK(sp.bars[b] == 0u, "bar drops to zero on silence");
    CHECK(sp.peaks[b] == held - 100u, "peak decays by exactly `decay` per update");

    uint32_t prev = sp.peaks[b];
    int monotone = 1;
    for (int u = 0; u < 12; u++) {
        tessera_spectrum_process(&sp, silence);
        if (sp.peaks[b] > prev) monotone = 0;
        prev = sp.peaks[b];
    }
    CHECK(monotone && sp.peaks[b] == 0u, "peak decays monotonically to zero");
}

/* ---- FFT tuner ------------------------------------------------------------- */

/* Feed `secs` worth of tone+noise and return the tuner's estimate. */
static float run_tuner(double freq, float noise_amp)
{
    tessera_ftuner_t t;
    tessera_ftuner_init(&t, N, SR, g_tw, g_fmem, g_cmem);
    static float blk[512];
    uint32_t total = (uint32_t)(SR / 2.0f);          /* half a second */
    uint32_t n = 0;
    while (n < total) {
        for (uint32_t i = 0; i < 512u; i++, n++)
            blk[i] = (float)(0.5 * sin(2.0 * M_PI * freq * n / (double)SR)) +
                     noise_amp * frand();
        tessera_ftuner_process(&t, blk, 512u);
    }
    return tessera_ftuner_hz(&t);
}

static double cents_err(float hz, double want)
{
    if (hz <= 0.0f) return 1e9;
    return 1200.0 * log2((double)hz / want);
}

static void test_tuner_accuracy(void)
{
    printf("- FFT tuner: within a cent or two, clean and detuned\n");
    float hz = run_tuner(440.0, 0.0f);
    printf("    440.00 Hz -> %.3f Hz (%.2f cents)\n", (double)hz, cents_err(hz, 440.0));
    CHECK(fabs(cents_err(hz, 440.0)) < 1.0, "440 Hz within a cent");

    hz = run_tuner(442.5, 0.0f);
    printf("    442.50 Hz -> %.3f Hz (%.2f cents)\n", (double)hz, cents_err(hz, 442.5));
    CHECK(fabs(cents_err(hz, 442.5)) < 1.0, "detuned 442.5 Hz within a cent");

    float cents;
    int note = tessera_fx_note_of(hz, &cents);
    CHECK(note == 69, "442.5 Hz maps to MIDI note 69 (A4)");
    CHECK(cents > 7.5f && cents < 12.5f, "cents offset ~ +9.8 from A440");
}

static void test_tuner_noise(void)
{
    printf("- FFT tuner: robust under noise that derails zero crossings\n");
    float hz = run_tuner(440.0, 0.3f);          /* tone 0.5, noise 0.3 */
    printf("    440 Hz + noise -> FFT tuner %.3f Hz (%.2f cents)\n",
           (double)hz, cents_err(hz, 440.0));
    CHECK(fabs(cents_err(hz, 440.0)) < 2.0, "FFT tuner within 2 cents under noise");

    /* The zero-crossing tuner on the same signal. */
    tessera_fx_tuner_t zc;
    tessera_fx_tuner_init(&zc, SR);
    static float blk[512];
    g_seed = 0x5eed5eedu;
    uint32_t n = 0;
    for (uint32_t b = 0; b < 40u; b++) {
        for (uint32_t i = 0; i < 512u; i++, n++)
            blk[i] = (float)(0.5 * sin(2.0 * M_PI * 440.0 * n / (double)SR)) +
                     0.3f * frand();
        tessera_fx_tuner_process(&zc, blk, 512u);
    }
    float zhz = tessera_fx_tuner_hz(&zc);
    printf("    440 Hz + noise -> zero-crossing tuner %.3f Hz (%.2f cents)\n",
           (double)zhz, cents_err(zhz, 440.0));
    CHECK(fabs(cents_err(hz, 440.0)) < fabs(cents_err(zhz, 440.0)),
          "FFT tuner beats the zero-crossing tuner under noise");
}

static void test_tuner_silence(void)
{
    printf("- FFT tuner: silence reads as no signal\n");
    tessera_ftuner_t t;
    tessera_ftuner_init(&t, N, SR, g_tw, g_fmem, g_cmem);
    static float z[512];
    memset(z, 0, sizeof z);
    for (uint32_t b = 0; b < 20u; b++)
        tessera_ftuner_process(&t, z, 512u);
    CHECK(tessera_ftuner_hz(&t) == 0.0f, "0 Hz on silence (gate holds)");
}

/* ---- OLED screens ----------------------------------------------------------- */

static int grid_has(const char *grid, const char *s)
{
    char line[OLED_COLS + 1];
    for (int r = 0; r < OLED_ROWS; r++) {
        memcpy(line, grid + r * OLED_COLS, OLED_COLS);
        line[OLED_COLS] = '\0';
        if (strstr(line, s)) return 1;
    }
    return 0;
}

static void test_oled_screens(void)
{
    printf("- OLED: spectrum and tuner screens render and navigate\n");
    oled_ui_t ui;
    oled_ui_init(&ui);

    CHECK(oled_ui_input(&ui, OLED_BTN_DOWN) == OLED_SCREEN_SPECTRUM,
          "HOME + down -> spectrum screen");

    /* Feed analyser-style per-mille levels: bar 3 full, bar 7 half, peak
     * above bar 7. */
    uint32_t bars[16] = { 0 }, peaks[16] = { 0 };
    bars[3] = 1000u; peaks[3] = 1000u;
    bars[7] = 500u;  peaks[7] = 900u;
    oled_ui_set_spectrum(&ui, bars, peaks, 16);

    char grid[OLED_ROWS * OLED_COLS];
    oled_ui_render(&ui, grid);
    CHECK(grid_has(grid, "Spectrum"), "title present");
    /* Bar 3 at 1000: column 3 filled to the top row below the title. */
    CHECK(grid[1 * OLED_COLS + 3] == '#' && grid[7 * OLED_COLS + 3] == '#',
          "full bar fills its column");
    /* Bar 7 at 500 -> ~4 of 7 rows: bottom filled, top empty, peak dash. */
    CHECK(grid[7 * OLED_COLS + 7] == '#' && grid[4 * OLED_COLS + 7] == '#',
          "half bar fills the lower rows");
    CHECK(grid[3 * OLED_COLS + 7] == ' ' || grid[3 * OLED_COLS + 7] == '-',
          "half bar leaves the upper rows to the peak marker");
    CHECK(grid[(OLED_ROWS - 6) * OLED_COLS + 7] == '-',
          "held peak renders as a dash above the bar");
    /* Untouched column stays empty. */
    CHECK(grid[7 * OLED_COLS + 12] == ' ', "silent bar leaves its column empty");

    CHECK(oled_ui_input(&ui, OLED_BTN_DOWN) == OLED_SCREEN_TUNER,
          "spectrum + down -> tuner screen");

    oled_ui_set_tuner(&ui, 69, -12, 4362u);        /* A4, -12 cents, 436.2 Hz */
    oled_ui_render(&ui, grid);
    CHECK(grid_has(grid, "Tuner"), "title present");
    CHECK(grid_has(grid, "A4"), "note name A4");
    CHECK(grid_has(grid, "-12c"), "cents text");
    CHECK(grid_has(grid, "436.2Hz"), "frequency text");
    /* Needle: centre '|' at column 10, '*' left of it for -12 cents. */
    int star = -1;
    for (int i = 0; i < OLED_COLS; i++)
        if (grid[4 * OLED_COLS + i] == '*') star = i;
    CHECK(star >= 0 && star < OLED_COLS / 2, "needle sits flat-side of centre");

    oled_ui_set_tuner(&ui, -1, 0, 0);
    oled_ui_render(&ui, grid);
    CHECK(grid_has(grid, "-listen-"), "no signal renders -listen-");

    CHECK(oled_ui_input(&ui, OLED_BTN_DOWN) == OLED_SCREEN_HOME,
          "tuner + down cycles back home");
    CHECK(oled_ui_input(&ui, OLED_BTN_UP) == OLED_SCREEN_TUNER &&
          oled_ui_input(&ui, OLED_BTN_BACK) == OLED_SCREEN_HOME,
          "HOME + up jumps straight to the tuner; back returns");
}

int main(void)
{
    printf("=== spectrum + FFT tuner + OLED host tests (issue #187) ===\n");
    tessera_fft_twiddles(g_tw, N);

    test_spectrum_tones();
    test_peak_hold();
    test_tuner_accuracy();
    test_tuner_noise();
    test_tuner_silence();
    test_oled_screens();

    if (g_fail) {
        printf("SPECTRUM TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("SPECTRUM TESTS: ALL PASS\n");
    return 0;
}
