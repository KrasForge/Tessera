/* plugins/example_sine/sine.c - sine-generating plugin (Issue #26, M5)
 *
 * Produces sound: writes a 440 Hz sine into the shared ring buffer so the host
 * can play it to the DAC.  Built twice - once normally, once with -DCRASH to
 * make it write a couple of blocks and then deliberately fault, so the host's
 * crash resilience can be exercised.
 */

#include "plugin_abi.h"
#include "audio_ringbuf.h"
#include "ring_contract.h"
#include "sine_gen.h"

#ifdef CRASH
#define SINE_BLOCKS 2u
#define KERNEL_ADDR 0x40000000ull
#else
#define SINE_BLOCKS RING_NBLOCKS
#endif

static sine_gen_t g_osc;

uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)block_size;
    audio_ring_hdr_t *ring = (audio_ring_hdr_t *)RING_VA;
    sine_gen_init(&g_osc, 440u, sample_rate);

    int16_t s16[RING_BLOCK * 2u];
    float   blk[RING_BLOCK * 2u];

    for (uint32_t b = 0; b < SINE_BLOCKS; b++) {
        sine_gen_fill(&g_osc, s16, RING_BLOCK);             /* int16 stereo */
        for (uint32_t i = 0; i < RING_BLOCK * 2u; i++)
            blk[i] = (float)s16[i] * (1.0f / 32768.0f);     /* -> float32   */
        arb_write(ring, blk, RING_BLOCK);                   /* shared memory */
    }

#ifdef CRASH
    *(volatile uint32_t *)KERNEL_ADDR = 0xDEADBEEFu;        /* fault -> killed */
#endif
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    for (uint32_t i = 0; i < n_frames; i++) { out_l[i] = in_l[i]; out_r[i] = in_r[i]; }
}

void plugin_set_param(uint32_t param_id, float value) { (void)param_id; (void)value; }
void plugin_destroy(void) { }
