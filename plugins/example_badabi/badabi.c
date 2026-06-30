/* plugins/example_badabi/badabi.c - wrong-ABI plugin (Issue #34, M8)
 *
 * A well-formed, self-contained AArch64 plugin whose only defect is that it
 * reports an incompatible ABI major version.  The loader runs the
 * plugin_abi_version() handshake before plugin_init() and must reject this
 * plugin (PM_EABI) before any of its real code runs.
 */

#include "plugin_abi.h"

/* Major version 2, which does not match the host's major (1). */
#define BADABI_VERSION ((2u << 16) | 0u)

uint32_t plugin_abi_version(void)
{
    return BADABI_VERSION;
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate;
    (void)block_size;
    return TESSERA_PLUGIN_OK;     /* must never be reached: rejected first */
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
