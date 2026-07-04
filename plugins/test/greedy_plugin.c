/* plugins/test/greedy_plugin.c - a memory-hungry plugin (Theme A: reliability)
 *
 * Declares a large .bss array, so its ELF PT_LOAD footprint is many pages.
 * It is a perfectly valid, well-behaved plugin - it just wants a lot of memory.
 * With a per-plugin memory quota set, the manager refuses it at load
 * (PM_EQUOTA) before committing a single frame; with no quota it loads and runs
 * like any other plugin.  Used by the memory-quota demo.
 */

#include "plugin_abi.h"

/* 256 KiB of zero-initialised state -> 64 pages of .bss in the PT_LOAD segment.
 * volatile + touched in init so the linker cannot discard it. */
#define BIG_BYTES (256u * 1024u)
static volatile unsigned char g_big[BIG_BYTES];

uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate; (void)block_size;
    g_big[0] = 1;
    g_big[BIG_BYTES - 1] = 2;
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    for (uint32_t i = 0; i < n_frames; i++) {   /* a harmless passthrough */
        out_l[i] = in_l[i];
        out_r[i] = in_r[i];
    }
}

void plugin_set_param(uint32_t param_id, float value) { (void)param_id; (void)value; }
void plugin_destroy(void) { }
