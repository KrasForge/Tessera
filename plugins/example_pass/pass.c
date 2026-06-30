/* plugins/example_pass/pass.c - passthrough plugin (Issue #24, M5)
 *
 * The minimal load target: copies input to output.  plugin_init does only
 * integer work (no FP, no allocation, no syscalls), so loading and entering it
 * exercises the loader and the EL0 trampoline cleanly.
 */

#include "plugin_abi.h"

static uint32_t g_sample_rate;
static uint32_t g_block_size;

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
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    for (uint32_t i = 0; i < n_frames; i++) {
        out_l[i] = in_l[i];
        out_r[i] = in_r[i];
    }
}

void plugin_set_param(uint32_t param_id, float value)
{
    (void)param_id;
    (void)value;
}

void plugin_destroy(void)
{
}
