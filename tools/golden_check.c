/* tools/golden_check.c - golden-audio regression harness (Theme M16, issue #168)
 *
 * Renders a linked reference plugin through its C ABI over a fixed, internally
 * generated deterministic input, applying an automation script, and compares the
 * output against a committed reference render.  A DSP change that alters the
 * sound moves the output far beyond the tolerance and fails the check; tiny
 * floating-point differences between compilers stay well within it.
 *
 * The comparison is the RMS of the sample-by-sample difference relative to the
 * reference's RMS - "did the sound change", robust across toolchains.  A real
 * algorithm change shifts this by whole percent; compiler jitter is orders of
 * magnitude smaller.  `--bless` writes the current render as the new reference.
 *
 * Usage:
 *   golden_check <automation.csv> <reference.pcm> [--bless]
 *
 * Linked per plugin (like the offline host).  Not real-time code: it runs on the
 * build host.
 */

#include "plugin_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The plugin's ABI exports (resolved at link time). */
int  plugin_init(uint32_t, uint32_t);
void plugin_set_param(uint32_t, float);
void plugin_process_block(const float *, const float *, float *, float *, uint32_t);
void plugin_destroy(void);

#define SR      48000u
#define FRAMES  16384u          /* render length (mono compared) */
#define BLOCK   128u
#define TOL     0.02            /* 2% RMS-difference ceiling */

typedef struct { uint32_t frame; uint32_t id; float value; } autom_t;

/* Deterministic stereo input: two tones plus a fixed LCG "noise", independent of
 * any plugin, so the output depends only on the plugin under test. */
static void gen_input(float *l, float *r, uint32_t n)
{
    uint32_t rng = 0x1234567u;
    for (uint32_t i = 0; i < n; i++) {
        rng = rng * 1664525u + 1013904223u;
        float noise = ((float)(rng >> 9) * (1.0f / 8388608.0f)) - 1.0f;   /* [-1,1) */
        double t = (double)i / SR;
        float s = 0.3f * (float)sin(2 * M_PI * 220.0 * t)
                + 0.2f * (float)sin(2 * M_PI * 3000.0 * t)
                + 0.05f * noise;
        l[i] = s;
        r[i] = s;
    }
}

static int16_t f2s(float v)
{
    int x = (int)lrintf(v * 32767.0f);
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    return (int16_t)x;
}

static int load_autom(const char *path, autom_t **out)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    autom_t *a = NULL; int n = 0, cap = 0;
    unsigned fr, id; float val;
    while (fscanf(f, "%u,%u,%f", &fr, &id, &val) == 3) {
        if (n == cap) { cap = cap ? cap * 2 : 16; a = realloc(a, cap * sizeof *a); }
        a[n].frame = fr; a[n].id = id; a[n].value = val; n++;
    }
    fclose(f);
    *out = a;
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <automation.csv> <reference.pcm> [--bless]\n", argv[0]); return 2; }
    const char *autom_path = argv[1];
    const char *ref_path   = argv[2];
    int bless = (argc > 3 && strcmp(argv[3], "--bless") == 0);

    static float il[FRAMES], ir[FRAMES];
    gen_input(il, ir, FRAMES);

    autom_t *autom = NULL;
    int n_autom = load_autom(autom_path, &autom);   /* 0 is fine (no automation) */

    static int16_t out[FRAMES];
    plugin_init(SR, BLOCK);
    int ai = 0;
    float ol[BLOCK], orr[BLOCK];
    for (uint32_t base = 0; base < FRAMES; base += BLOCK) {
        uint32_t n = FRAMES - base; if (n > BLOCK) n = BLOCK;
        while (ai < n_autom && autom[ai].frame < base + n) {
            plugin_set_param(autom[ai].id, autom[ai].value);
            ai++;
        }
        plugin_process_block(il + base, ir + base, ol, orr, n);
        for (uint32_t i = 0; i < n; i++) out[base + i] = f2s(ol[i]);   /* left channel */
    }
    plugin_destroy();
    free(autom);

    if (bless) {
        FILE *f = fopen(ref_path, "wb");
        if (!f) { perror("open reference for write"); return 2; }
        fwrite(out, sizeof(int16_t), FRAMES, f);
        fclose(f);
        printf("blessed %s (%u samples)\n", ref_path, FRAMES);
        return 0;
    }

    FILE *f = fopen(ref_path, "rb");
    if (!f) { fprintf(stderr, "golden: missing reference %s (run `make golden-bless`)\n", ref_path); return 2; }
    static int16_t ref[FRAMES];
    size_t got = fread(ref, sizeof(int16_t), FRAMES, f);
    fclose(f);
    if (got != FRAMES) { fprintf(stderr, "golden: short reference %s\n", ref_path); return 2; }

    double diff2 = 0.0, ref2 = 0.0;
    for (uint32_t i = 0; i < FRAMES; i++) {
        double d = (double)out[i] - (double)ref[i];
        diff2 += d * d;
        ref2  += (double)ref[i] * (double)ref[i];
    }
    double ratio = (ref2 > 0.0) ? sqrt(diff2 / ref2) : sqrt(diff2);
    int ok = ratio <= TOL;
    printf("golden %s: rms-diff ratio %.5f (<= %.2f) -> %s\n",
           ref_path, ratio, TOL, ok ? "OK" : "CHANGED");
    return ok ? 0 : 1;
}
