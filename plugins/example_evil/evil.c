/* plugins/example_evil/evil.c - misbehaving plugin (Issue #24, M5)
 *
 * Exists only to prove isolation: plugin_init writes to a kernel address.  The
 * loader maps the kernel's pages into the plugin space as EL1-only, so this
 * store faults from EL0 and the host kills the plugin (issue #14) - it can
 * never corrupt kernel memory.
 */

#include "plugin_abi.h"

/* Start of kernel RAM on the QEMU 'virt' board: mapped, but EL1-only. */
#define KERNEL_ADDR 0x40000000ull

uint32_t plugin_abi_version(void)
{
    return TESSERA_PLUGIN_ABI_VERSION;
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate;
    (void)block_size;
    *(volatile uint32_t *)KERNEL_ADDR = 0xDEADBEEFu;   /* -> EL0 data abort */
    return TESSERA_PLUGIN_OK;                           /* never reached     */
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    (void)in_l; (void)in_r; (void)out_l; (void)out_r; (void)n_frames;
}

void plugin_set_param(uint32_t param_id, float value)
{
    (void)param_id;
    (void)value;
}

void plugin_destroy(void)
{
}
