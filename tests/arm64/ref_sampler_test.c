/* tests/arm64/ref_sampler_test.c - host test for the reference sampler plugin
 * (Theme M15, issue #167).
 *
 * Build/run via:  make test-arm-ref-sampler
 */

#include "tessera.h"
#include "presets.h"
#include "plugin_abi.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

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

#define BLOCK 128

static float rms(const float *x, int n)
{
    double a = 0; for (int i = 0; i < n; i++) a += (double)x[i] * x[i];
    return (float)sqrt(a / n);
}

static void render(float *out, int blocks)
{
    float il[BLOCK] = {0}, ir[BLOCK] = {0}, ol[BLOCK], orr[BLOCK];
    for (int b = 0; b < blocks; b++) {
        plugin_process_block(il, ir, ol, orr, BLOCK);
        for (int i = 0; i < BLOCK; i++) out[b * BLOCK + i] = ol[i];
    }
}

static void test_plays_and_gates(void)
{
    printf("- the sampler plays the bundled sample and gates\n");
    plugin_init(48000, BLOCK);
    static float out[BLOCK * 32];
    render(out, 32);
    CHECK(rms(out, BLOCK * 32) > 0.05f, "sampler produces audio");

    /* Gate off -> silence; gate on -> audio again. */
    plugin_set_param(1, 0.0f);
    render(out, 16);
    CHECK(rms(out, BLOCK * 16) < 1e-4f, "gate off silences the output");
    plugin_set_param(1, 1.0f);
    render(out, 16);
    CHECK(rms(out, BLOCK * 16) > 0.05f, "gate on resumes audio");
    plugin_destroy();
}

static void test_pitch_bounded(void)
{
    printf("- pitch change stays bounded (streaming ring never overruns)\n");
    plugin_init(48000, BLOCK);
    plugin_set_param(0, 2.0f);   /* octave up: consumes the ring twice as fast */
    static float out[BLOCK * 64];
    render(out, 64);
    /* It must keep producing (refill keeps up) and stay in range. */
    CHECK(rms(out, BLOCK * 64) > 0.05f, "octave-up still produces audio (refill keeps up)");
    float peak = 0;
    for (int i = 0; i < BLOCK * 64; i++) if (fabsf(out[i]) > peak) peak = fabsf(out[i]);
    CHECK(peak <= 1.001f, "output stays within range");
    plugin_destroy();
}

static void test_embedded_presets(void)
{
    printf("- embedded presets parse\n");
    unsigned int len = 0;
    const unsigned char *blob = plugin_preset_blob(&len);
    preset_table_t t;
    CHECK(presets_open(&t, blob, len) == 0, "the .tessera.presets blob is valid");
    CHECK(presets_count(&t) == 2, "two factory presets");
    preset_info_t p;
    CHECK(presets_get(&t, 0, &p) == 0 && strcmp(p.name, "Normal") == 0, "preset 0 is Normal");
    CHECK(presets_get(&t, 1, &p) == 0 && strcmp(p.name, "OctaveUp") == 0, "preset 1 is OctaveUp");
}

int main(void)
{
    printf("=== Tessera reference sampler plugin test (M15, #167) ===\n");
    CHECK(plugin_abi_version() == TESSERA_PLUGIN_ABI_VERSION, "plugin reports the SDK ABI version");
    test_plays_and_gates();
    test_pitch_bounded();
    test_embedded_presets();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
