/* plugins/example_crasher/crasher.c - shared-ring producer that crashes
 * mid-stream (Issue #25, M5).
 *
 * Writes a couple of valid blocks into the shared ring and then crashes by
 * touching kernel memory.  The host must survive: it drains the valid frames
 * the crasher managed to publish, then sees the ring go quiet and fills the
 * rest with silence rather than faulting or replaying stale audio.
 */

#include "plugin_abi.h"
#include "audio_ringbuf.h"
#include "ring_contract.h"

#define CRASH_BLOCKS 2u
#define KERNEL_ADDR  0x40000000ull   /* mapped EL1-only -> faults from EL0 */

uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate;
    (void)block_size;

    audio_ring_hdr_t *ring = (audio_ring_hdr_t *)RING_VA;
    float block[RING_BLOCK * 2u];

    for (uint32_t b = 0; b < CRASH_BLOCKS; b++) {
        for (uint32_t i = 0; i < RING_BLOCK; i++) {
            float v = (float)(b * RING_BLOCK + i);
            block[i * 2u + 0] = v;
            block[i * 2u + 1] = v + 0.5f;
        }
        arb_write(ring, block, RING_BLOCK);
    }

    *(volatile uint32_t *)KERNEL_ADDR = 0xDEADBEEFu;   /* crash: EL0 fault */
    return TESSERA_PLUGIN_OK;                           /* never reached    */
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    for (uint32_t i = 0; i < n_frames; i++) { out_l[i] = in_l[i]; out_r[i] = in_r[i]; }
}

void plugin_set_param(uint32_t param_id, float value) { (void)param_id; (void)value; }
void plugin_destroy(void) { }
