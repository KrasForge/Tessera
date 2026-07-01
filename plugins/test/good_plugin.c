/* plugins/test/good_plugin.c - well-behaved plugin (Issue #36, M8 demo)
 *
 * A clean sine generator.  Its plugin_process_block() writes a continuous
 * 440 Hz tone into its output buffers every block and never touches anything
 * outside its sandbox, so it keeps producing audio uninterrupted while the
 * misbehaving plugins around it are caught and killed.
 */

#include "plugin_abi.h"
#include "sine_gen.h"

static sine_gen_t g_osc;

uint32_t plugin_abi_version(void)
{
    return TESSERA_PLUGIN_ABI_VERSION;
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)block_size;
    sine_gen_init(&g_osc, 440u, sample_rate);
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    (void)in_l;
    (void)in_r;
    for (uint32_t i = 0; i < n_frames; i++) {
        float s = (float)sine_gen_next(&g_osc) * (1.0f / 32768.0f);
        out_l[i] = s;
        out_r[i] = s;
    }
}

void plugin_set_param(uint32_t param_id, float value)
{
    (void)param_id; (void)value;
}

void plugin_destroy(void)
{
}
