/* plugins/example_gain/gain.c - reference Tessera plugin (Issue #23, M5)
 *
 * The simplest possible plugin that exercises the whole ABI: a stereo gain.
 * It is deliberately written against ONLY <plugin_abi.h> (no kernel headers),
 * so it demonstrates that a third party can build a plugin with a stock
 * aarch64 toolchain.  process_block is real-time-safe: a bounded loop of
 * multiplies, no allocation, no locks, no syscalls.
 */

#include "plugin_abi.h"

/* Parameters this plugin understands. */
#define PARAM_GAIN 0u            /* linear gain, default 1.0 */

/* Plugin state.  Static storage only; no allocation needed for a gain. */
static uint32_t g_sample_rate;
static uint32_t g_block_size;
static volatile float g_gain = 1.0f;   /* updated wait-free from set_param */

uint32_t plugin_abi_version(void)
{
    return TESSERA_PLUGIN_ABI_VERSION;
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    if (sample_rate == 0 || block_size == 0)
        return TESSERA_PLUGIN_EINVAL;
    g_sample_rate = sample_rate;
    g_block_size  = block_size;
    g_gain        = 1.0f;
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    const float gain = g_gain;          /* single load: wait-free snapshot */
    for (uint32_t i = 0; i < n_frames; i++) {
        out_l[i] = in_l[i] * gain;
        out_r[i] = in_r[i] * gain;
    }
}

void plugin_set_param(uint32_t param_id, float value)
{
    if (param_id == PARAM_GAIN)
        g_gain = value;                 /* single store: real-time safe */
    /* unknown params are ignored, per the ABI */
}

void plugin_destroy(void)
{
    g_gain = 1.0f;                       /* nothing allocated to release */
}
