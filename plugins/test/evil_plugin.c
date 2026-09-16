/* plugins/test/evil_plugin.c - actively malicious plugin (Issue #36, M8 demo)
 *
 * Offers two attacks from inside plugin_process_block(), exercised in
 * separate fresh instances by the resilience test:
 *
 *   1. A syscall (SVC) from the audio path.  A sandboxed plugin may only reach
 *      the kernel through its controlled trampoline; an SVC from the plugin's
 *      own body is a protocol violation, so the kernel kills it instead of
 *      servicing the call (issue #35).  This is the attack we let fire first,
 *      because it exercises the distinctive audio-path protection.
 *   2. A wild write to a kernel address (0xFFFF000000000000, the TTBR1 half).
 *      EL0 cannot reach kernel memory, so the MMU faults and the plugin is
 *      killed. Test-only init(sample_rate=0) selects this attack before SVC;
 *      normal init selects the illegal SVC instead.
 *
 * Either way the plugin is killed with logged fault info and the engine and
 * the other plugins keep running.
 */

#include "plugin_abi.h"

/* Test-only mode: init with sample_rate=0 selects the kernel-write attack
 * in a fresh process; normal init selects the forbidden-SVC attack. */
static int g_write_attack;

uint32_t plugin_abi_version(void)
{
    return TESSERA_PLUGIN_ABI_VERSION;
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)block_size;
    g_write_attack = (sample_rate == 0);
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    (void)in_l; (void)in_r; (void)out_l; (void)out_r; (void)n_frames;

    if (g_write_attack)
        *(volatile uint64_t *)0xFFFF000000000000ull = 0xDEADu;

    /* Attack 1: a forbidden syscall from the audio path. */
    register long x8 __asm__("x8") = 1;     /* SYS_WRITE */
    __asm__ volatile("svc #0" :: "r"(x8) : "x0", "memory");

    /* Attack 2 (unreachable): a wild write to kernel memory. */
    *(volatile uint64_t *)0xFFFF000000000000ull = 0xDEADu;
}

void plugin_set_param(uint32_t param_id, float value)
{
    (void)param_id; (void)value;
}

void plugin_destroy(void)
{
}
