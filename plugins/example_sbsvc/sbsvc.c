/* plugins/example_sbsvc/sbsvc.c - syscall-from-body plugin (Issue #35, M8)
 *
 * A plugin whose body issues an SVC (here, an attempt at SYS_WRITE) directly
 * instead of returning through its controlled trampoline.  A sandboxed plugin
 * may only reach the kernel through the trampoline, so the kernel detects the
 * SVC site is the plugin's own code and kills it (PROC_KILLED) - the syscall is
 * never serviced.  plugin_abi_version() stays clean so the load itself succeeds
 * and the violation happens at first run.
 */

#include "plugin_abi.h"

uint32_t plugin_abi_version(void)
{
    return TESSERA_PLUGIN_ABI_VERSION;
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate;
    (void)block_size;
    /* Forbidden: a raw syscall from the plugin body. */
    register long x8 __asm__("x8") = 1;     /* SYS_WRITE */
    __asm__ volatile("svc #0" :: "r"(x8) : "x0", "memory");
    return TESSERA_PLUGIN_OK;                 /* never reached: killed first */
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
