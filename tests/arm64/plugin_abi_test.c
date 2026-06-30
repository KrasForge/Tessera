/* tests/arm64/plugin_abi_test.c - host unit tests for the plugin ABI
 * (Issue #23).
 *
 * Compiles the reference plugin (plugins/example_gain/gain.c) against only
 * <plugin_abi.h> and drives it through the ABI to confirm the version
 * handshake, the init contract, real-time-safe parameter updates, and correct
 * block processing.
 *
 * Build/run via:  make test-arm-plugin-abi
 */

#include "plugin_abi.h"

#include <stdio.h>
#include <math.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static int feq(float a, float b) { return fabsf(a - b) < 1e-6f; }

int main(void)
{
    printf("=== Tessera plugin-ABI tests (issue #23) ===\n");

    /* Version handshake (callable before init). */
    CHECK(plugin_abi_version() == TESSERA_PLUGIN_ABI_VERSION,
          "plugin_abi_version() returns the expected constant");
    CHECK((TESSERA_PLUGIN_ABI_VERSION >> 16) == 1u, "ABI major version is 1");

    /* init contract. */
    CHECK(plugin_init(48000, 128) == TESSERA_PLUGIN_OK, "init(48k,128) succeeds");
    CHECK(plugin_init(0, 128) == TESSERA_PLUGIN_EINVAL, "init rejects zero sample rate");
    CHECK(plugin_init(48000, 0) == TESSERA_PLUGIN_EINVAL, "init rejects zero block size");

    /* Re-init cleanly for processing. */
    plugin_init(48000, 4);

    float in_l[4]  = {1.0f, -2.0f, 0.5f, 4.0f};
    float in_r[4]  = {2.0f,  1.0f, -1.0f, 0.25f};
    float out_l[4] = {0}, out_r[4] = {0};

    /* Default gain is unity. */
    plugin_process_block(in_l, in_r, out_l, out_r, 4);
    int unity = 1;
    for (int i = 0; i < 4; i++)
        if (!feq(out_l[i], in_l[i]) || !feq(out_r[i], in_r[i])) unity = 0;
    CHECK(unity, "default gain passes audio through unchanged");

    /* Parameter update applies on the next block. */
    plugin_set_param(0u, 2.0f);          /* PARAM_GAIN */
    plugin_process_block(in_l, in_r, out_l, out_r, 4);
    int doubled = 1;
    for (int i = 0; i < 4; i++)
        if (!feq(out_l[i], in_l[i] * 2.0f) || !feq(out_r[i], in_r[i] * 2.0f)) doubled = 0;
    CHECK(doubled, "set_param(GAIN,2.0) doubles the signal");

    /* Unknown parameter ids are ignored (no crash, no effect). */
    plugin_set_param(999u, 7.0f);
    plugin_process_block(in_l, in_r, out_l, out_r, 4);
    CHECK(feq(out_l[0], in_l[0] * 2.0f), "unknown param id is ignored");

    /* Zero-length block is a no-op. */
    plugin_process_block(in_l, in_r, out_l, out_r, 0);
    CHECK(1, "zero-frame block does not crash");

    plugin_destroy();
    CHECK(1, "destroy() completes");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
