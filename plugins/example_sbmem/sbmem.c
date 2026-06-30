/* plugins/example_sbmem/sbmem.c - out-of-sandbox memory access (Issue #35, M8)
 *
 * Models a plugin that uses memory it was never granted - e.g. a pointer from
 * a malloc() that does not exist in its sandbox.  It writes to a user virtual
 * address that the loader never mapped, so the MMU raises a data abort from
 * EL0 and the host kills the plugin.  It can reach nothing but its own pages
 * and the shared audio buffer; everything else is unmapped and faults.
 */

#include "plugin_abi.h"

/* A "heap" pointer in the plugin's own VA range that was never mapped. */
#define UNMAPPED_HEAP (0x8000000000ULL + 0x40000000ULL)

uint32_t plugin_abi_version(void)
{
    return TESSERA_PLUGIN_ABI_VERSION;
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate;
    (void)block_size;
    *(volatile uint32_t *)UNMAPPED_HEAP = 0xC0FFEEu;   /* -> EL0 data abort */
    return TESSERA_PLUGIN_OK;                            /* never reached    */
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    (void)in_l; (void)in_r; (void)out_l; (void)out_r; (void)n_frames;
}

void plugin_set_param(uint32_t param_id, float value)
{
    (void)param_id; (void)value;
}

void plugin_destroy(void)
{
}
