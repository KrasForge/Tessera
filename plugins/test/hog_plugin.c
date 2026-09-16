/* plugins/test/hog_plugin.c - CPU-hogging plugin (Issue #78, M12)
 *
 * Renders three blocks, then dirties its output and spins forever. The MMU
 * cannot catch this
 * (no bad access) and the SVC gate cannot either (no syscall) - only the
 * per-plugin CPU budget can: the kernel's budget timer preempts it at its
 * budget boundary, the host mutes it, and after repeated offences kills it.
 * The third leg of the sandbox: memory (crash), syscalls (evil), time (hog).
 */

#include "plugin_abi.h"

static uint32_t g_calls;

uint32_t plugin_abi_version(void)
{
    return TESSERA_PLUGIN_ABI_VERSION;
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate; (void)block_size;
    g_calls = 0;
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    (void)in_l; (void)in_r;
    for (uint32_t i = 0; i < n_frames; i++) {
        ((volatile float *)out_l)[i] = 0.75f;
        ((volatile float *)out_r)[i] = -0.75f;
    }
    /* Behave for three blocks, then hold the core after dirtying the output. */
    if (g_calls++ >= 3u)
        for (;;)
            __asm__ volatile("");
}

void plugin_set_param(uint32_t param_id, float value)
{
    (void)param_id; (void)value;
}

void plugin_destroy(void)
{
}
