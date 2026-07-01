/* plugins/test/crash_plugin.c - crashing plugin (Issue #36, M8 demo)
 *
 * Dereferences a null pointer inside plugin_process_block(), causing a data
 * abort from EL0.  The MMU traps the access, the host kills the plugin
 * process, and the audio engine and the other plugins keep running - a buggy
 * plugin cannot take the system down.
 */

#include "plugin_abi.h"

uint32_t plugin_abi_version(void)
{
    return TESSERA_PLUGIN_ABI_VERSION;
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate; (void)block_size;
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    (void)in_l; (void)in_r; (void)out_l; (void)out_r; (void)n_frames;
    volatile float *null_ptr = (volatile float *)0;   /* NULL */
    *null_ptr = 1.0f;                                  /* -> EL0 data abort */
}

void plugin_set_param(uint32_t param_id, float value)
{
    (void)param_id; (void)value;
}

void plugin_destroy(void)
{
}
