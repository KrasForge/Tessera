/* plugins/test/blip_plugin.c - transiently-hogging plugin (Issue #78, M12)
 *
 * Behaves for two blocks, spins forever on blocks 3 and 4, then behaves
 * again.  Exercises the budget policy's forgiveness: two consecutive
 * offences earn two mutes but - below the kill threshold - no kill, and a
 * clean block resets the streak.  When behaving it writes a non-zero marker
 * block so the host can verify it is audible again after the mutes.
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
    g_calls++;

    if (g_calls == 3u || g_calls == 4u)
        for (;;)
            __asm__ volatile("");  /* the naughty streak: blocks 3 and 4 */

    for (uint32_t i = 0; i < n_frames; i++) {
        out_l[i] = 0.25f;          /* audible marker: "I am behaving"    */
        out_r[i] = 0.25f;
    }
}

void plugin_set_param(uint32_t param_id, float value)
{
    (void)param_id; (void)value;
}

void plugin_destroy(void)
{
}
