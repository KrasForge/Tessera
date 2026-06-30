/* tests/arm64/filter_test.c - host unit tests for the reference low-pass
 * filter plugin (Issue #29).
 *
 * Drives the plugin through its ABI and measures its behaviour on real signals:
 * low frequencies pass, high frequencies are attenuated, raising the cutoff
 * lets more highs through, the output is stable, and changing the cutoff in
 * real time does not introduce a discontinuity beyond the filter transient.
 *
 * The plugin uses no libm; this test uses sinf only to synthesise inputs.
 *
 * Build/run via:  make test-arm-filter
 */

#define _GNU_SOURCE
#include "plugin_abi.h"

#include <stdint.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define SR    48000u
#define BLK   256u

/* Push a `freq` Hz sine through the filter for `nframes` and return the RMS of
 * the (settled) output relative to the input amplitude. */
static float measure_gain(float freq, uint32_t nframes)
{
    float in_l[BLK], in_r[BLK], out_l[BLK], out_r[BLK];
    double phase = 0.0, dp = 2.0 * M_PI * freq / SR;
    double sumsq = 0.0;
    uint32_t counted = 0, done = 0;

    while (done < nframes) {
        uint32_t n = (nframes - done < BLK) ? (nframes - done) : BLK;
        for (uint32_t i = 0; i < n; i++) {
            in_l[i] = in_r[i] = (float)sin(phase);
            phase += dp;
        }
        plugin_process_block(in_l, in_r, out_l, out_r, n);
        /* Skip the first half (settling transient); measure the rest. */
        for (uint32_t i = 0; i < n; i++) {
            if (done + i >= nframes / 2) { sumsq += (double)out_l[i] * out_l[i]; counted++; }
        }
        done += n;
    }
    /* Input is unit-amplitude sine -> RMS 0.707; report output RMS / 0.707. */
    double rms = sqrt(sumsq / (counted ? counted : 1));
    return (float)(rms / 0.70710678);
}

int main(void)
{
    printf("=== Tessera low-pass filter tests (issue #29) ===\n");

    CHECK(plugin_abi_version() == TESSERA_PLUGIN_ABI_VERSION, "ABI version constant");
    CHECK(plugin_init(SR, BLK) == TESSERA_PLUGIN_OK, "init ok");
    CHECK(plugin_init(0, BLK) == TESSERA_PLUGIN_EINVAL, "init rejects zero rate");

    /* Default cutoff 1 kHz: a 100 Hz tone passes, a 12 kHz tone is killed. */
    plugin_init(SR, BLK);
    plugin_set_param(0, 1000.0f);
    float low_gain  = measure_gain(100.0f, 8 * BLK);
    plugin_init(SR, BLK);
    plugin_set_param(0, 1000.0f);
    float high_gain = measure_gain(12000.0f, 8 * BLK);
    printf("    cutoff=1kHz: gain(100Hz)=%.3f gain(12kHz)=%.4f\n", low_gain, high_gain);
    CHECK(low_gain > 0.7f, "low frequency passes (gain near unity)");
    CHECK(high_gain < 0.1f, "high frequency strongly attenuated");
    CHECK(high_gain < low_gain * 0.2f, "clear low-pass shape");

    /* Raising the cutoff lets more of the 12 kHz through. */
    plugin_init(SR, BLK);
    plugin_set_param(0, 12000.0f);
    float high_gain_open = measure_gain(12000.0f, 8 * BLK);
    printf("    cutoff=12kHz: gain(12kHz)=%.3f\n", high_gain_open);
    CHECK(high_gain_open > high_gain * 2.0f, "raising cutoff passes more highs");

    /* Stability: no blow-up / NaN over a long run. */
    plugin_init(SR, BLK);
    plugin_set_param(0, 2000.0f);
    float g = measure_gain(500.0f, 200 * BLK);
    CHECK(g == g && g < 4.0f, "filter is stable (bounded, finite output)");

    /* Real-time cutoff change is continuous: process a block, change the
     * cutoff, process the next sample, and confirm no large jump at the seam
     * beyond the signal's own slope. */
    plugin_init(SR, BLK);
    plugin_set_param(0, 800.0f);
    float il[BLK], ir[BLK], ol[BLK], orr[BLK];
    double ph = 0.0, dp = 2.0 * M_PI * 300.0 / SR;
    for (uint32_t i = 0; i < BLK; i++) { il[i] = ir[i] = (float)sin(ph); ph += dp; }
    plugin_process_block(il, ir, ol, orr, BLK);
    float last = ol[BLK - 1];
    plugin_set_param(0, 4000.0f);                 /* big cutoff jump */
    float one_in = (float)sin(ph);
    float one_out;
    { float a = one_in, b = one_in; float oa, ob;
      plugin_process_block(&a, &b, &oa, &ob, 1); one_out = oa; }
    float step = one_out - last; if (step < 0) step = -step;
    printf("    seam step on cutoff change = %.4f\n", step);
    CHECK(step < 0.2f, "cutoff change does not cause a large discontinuity");

    plugin_destroy();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
