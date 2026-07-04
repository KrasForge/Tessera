/* tests/arm64/ref_synth_test.c - host test for the reference FM synth plugin
 * (Theme M15, issue #167).  Drives the plugin through its C ABI (as the offline
 * host does) and checks it makes sound at the right pitch, plus its embedded
 * presets parse and apply.
 *
 * Build/run via:  make test-arm-ref-synth
 */

#include "tessera.h"
#include "presets.h"
#include "plugin_abi.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* The plugin's ABI exports (linked from plugins/synth_fm/main.c). */
int      plugin_init(uint32_t, uint32_t);
void     plugin_set_param(uint32_t, float);
void     plugin_process_block(const float *, const float *, float *, float *, uint32_t);
void     plugin_destroy(void);
const unsigned char *plugin_preset_blob(unsigned int *);

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define SR 48000.0f
#define BLOCK 128

/* Goertzel magnitude at `hz` over `n` samples. */
static double mag_at(const float *x, int n, double hz)
{
    double w = 2.0 * 3.14159265358979 * hz / SR, c = 2.0 * cos(w), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) { double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    double re = s1 - s2 * cos(w), im = s2 * sin(w);
    return sqrt(re * re + im * im) / n;
}

/* Render `blocks` blocks into `out` (mono, left channel). */
static void render(float *out, int blocks)
{
    float il[BLOCK] = {0}, ir[BLOCK] = {0}, ol[BLOCK], orr[BLOCK];
    for (int b = 0; b < blocks; b++) {
        plugin_process_block(il, ir, ol, orr, BLOCK);
        for (int i = 0; i < BLOCK; i++) out[b * BLOCK + i] = ol[i];
    }
}

static void test_makes_sound(void)
{
    printf("- the FM synth makes sound at the played pitch\n");
    plugin_init((uint32_t)SR, BLOCK);
    plugin_set_param(2, 1.0f);   /* FM ratio 1 */
    plugin_set_param(3, 1.0f);   /* modest index for a clear fundamental */
    plugin_set_param(0, 69.0f);  /* note-on A4 = 440 Hz */

    static float out[BLOCK * 64];
    render(out, 64);

    float peak = 0;
    for (int i = 0; i < BLOCK * 64; i++) if (fabsf(out[i]) > peak) peak = fabsf(out[i]);
    CHECK(peak > 0.05f, "note-on produces audio");
    CHECK(mag_at(out + 2048, BLOCK * 64 - 2048, 440.0) > 0.02, "energy at 440 Hz (A4)");

    /* Note-off then let the release finish -> silence. */
    plugin_set_param(1, 69.0f);
    static float tail[BLOCK * 400];
    render(tail, 400);
    float endpeak = 0;
    for (int i = BLOCK * 380; i < BLOCK * 400; i++) if (fabsf(tail[i]) > endpeak) endpeak = fabsf(tail[i]);
    CHECK(endpeak < 0.01f, "note-off releases to silence");
    plugin_destroy();
}

static void test_embedded_presets(void)
{
    printf("- embedded presets parse and apply\n");
    unsigned int len = 0;
    const unsigned char *blob = plugin_preset_blob(&len);
    preset_table_t t;
    CHECK(presets_open(&t, blob, len) == 0, "the .tessera.presets blob is valid");
    CHECK(presets_count(&t) == 2, "two factory presets");

    preset_info_t p;
    CHECK(presets_get(&t, 0, &p) == 0 && strcmp(p.name, "Bell") == 0, "preset 0 is Bell");
    CHECK(presets_get(&t, 1, &p) == 0 && strcmp(p.name, "Bass") == 0, "preset 1 is Bass");
    CHECK(p.n_params == 2, "Bass has 2 params (ratio, index)");

    /* Apply the Bell preset through the ABI and confirm it still sounds. */
    plugin_init((uint32_t)SR, BLOCK);
    presets_get(&t, 0, &p);
    for (int i = 0; i < p.n_params; i++) {
        uint32_t id, bits; preset_param(&p, i, &id, &bits);
        float v; memcpy(&v, &bits, 4);
        plugin_set_param(id, v);
    }
    plugin_set_param(0, 60.0f);   /* middle C */
    static float out[BLOCK * 32];
    render(out, 32);
    float peak = 0;
    for (int i = 0; i < BLOCK * 32; i++) if (fabsf(out[i]) > peak) peak = fabsf(out[i]);
    CHECK(peak > 0.05f, "the Bell preset renders audio");
    plugin_destroy();
}

int main(void)
{
    printf("=== Tessera reference FM-synth plugin test (M15, #167) ===\n");
    CHECK(plugin_abi_version() == TESSERA_PLUGIN_ABI_VERSION, "plugin reports the SDK ABI version");
    test_makes_sound();
    test_embedded_presets();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
