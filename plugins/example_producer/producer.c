/* plugins/example_producer/producer.c - shared-ring producer plugin
 * (Issue #25, M5).
 *
 * Writes a known ramp into the shared audio ring buffer that the host mapped at
 * RING_VA - directly, with no syscalls.  This is the "zero kernel involvement
 * per block" data path: the only SVC the whole run makes is the final exit from
 * the entry trampoline.
 */

#include "plugin_abi.h"
#include "audio_ringbuf.h"
#include "ring_contract.h"

uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate;
    (void)block_size;

    audio_ring_hdr_t *ring = (audio_ring_hdr_t *)RING_VA;
    float block[RING_BLOCK * 2u];

    for (uint32_t b = 0; b < RING_NBLOCKS; b++) {
        for (uint32_t i = 0; i < RING_BLOCK; i++) {
            float v = (float)(b * RING_BLOCK + i);
            block[i * 2u + 0] = v;            /* left  = frame index       */
            block[i * 2u + 1] = v + 0.5f;     /* right = index + 0.5       */
        }
        arb_write(ring, block, RING_BLOCK);   /* shared memory: no syscall */
    }
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    for (uint32_t i = 0; i < n_frames; i++) { out_l[i] = in_l[i]; out_r[i] = in_r[i]; }
}

void plugin_set_param(uint32_t param_id, float value) { (void)param_id; (void)value; }
void plugin_destroy(void) { }
