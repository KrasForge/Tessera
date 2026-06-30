/* plugins/example_effect/effect.c - in-line effect plugin (Issue #27, M6)
 *
 * A graph "effect" node: it reads audio from its input ring (RING_IN_VA, fed by
 * an upstream node), applies a gain of 0.5, and writes the result to its output
 * ring (RING_VA, consumed by the downstream node).  Because it consumes its
 * input, it only produces real output when its upstream producer has already
 * run - which is exactly what the graph's topological order guarantees.
 */

#include "plugin_abi.h"
#include "audio_ringbuf.h"
#include "ring_contract.h"

#define EFFECT_GAIN 0.5f

uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate;
    (void)block_size;

    audio_ring_hdr_t *in  = (audio_ring_hdr_t *)RING_IN_VA;
    audio_ring_hdr_t *out = (audio_ring_hdr_t *)RING_VA;
    float blk[RING_BLOCK * 2u];

    for (uint32_t b = 0; b < RING_NBLOCKS; b++) {
        arb_read(in, blk, RING_BLOCK);                 /* upstream audio   */
        for (uint32_t i = 0; i < RING_BLOCK * 2u; i++)
            blk[i] *= EFFECT_GAIN;                      /* apply the effect */
        arb_write(out, blk, RING_BLOCK);               /* downstream audio */
    }
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    for (uint32_t i = 0; i < n_frames; i++) {
        out_l[i] = in_l[i] * EFFECT_GAIN;
        out_r[i] = in_r[i] * EFFECT_GAIN;
    }
}

void plugin_set_param(uint32_t param_id, float value) { (void)param_id; (void)value; }
void plugin_destroy(void) { }
