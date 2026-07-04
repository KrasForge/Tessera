/* plugins/sampler/main.c - reference sampler plugin (Theme M15, issue #167)
 *
 * A worked sampler built on the SDK streaming sampler (issue #165): it plays a
 * short bundled sample, looped, at a controllable pitch.  Because the sampler
 * reads through a fixed ring, its memory is bounded no matter how the sample is
 * driven - the isolation guarantee that motivates the design.  Factory presets
 * are embedded in the ELF (issue #127).
 *
 * Control (also scriptable through the offline host, issue #128):
 *   param 0 = pitch ratio (1.0 = original, 2.0 = octave up)
 *   param 1 = gate        (>= 0.5 plays, else silent)
 *
 * In a real deployment the bundled sample would instead be streamed from the SD
 * card by the host; here it is a small embedded waveform so the plugin is
 * self-contained and deterministically renderable.  Real-time-safe: the ring
 * top-up in process_block is bounded (no allocation, no locks, no syscalls).
 */

#include "tessera.h"

#define PARAM_PITCH 0u
#define PARAM_GATE  1u

/* A short bundled sample: two cycles of a decaying sine (a plucky "ping"). */
#define SAMPLE_LEN 512
static float g_sample[SAMPLE_LEN];

#define RING_CAP 1024
static float g_ring[RING_CAP];
static tessera_sampler_t g_samp;
static uint32_t g_src_pos;      /* next sample index to feed (loops)  */
static int      g_gate = 1;

/* ---- embedded factory presets (.tessera.presets, issue #127) ------------- */
struct __attribute__((packed)) preset_entry {
    char     name[24];
    uint16_t n_params;
    uint16_t reserved;
    struct { uint32_t id, bits; } p[1];
};
struct __attribute__((packed)) preset_blob {
    uint32_t magic;
    uint16_t version;
    uint16_t n_presets;
    struct preset_entry e[2];
};
static const struct preset_blob g_presets
    __attribute__((section(".tessera.presets"), used)) = {
    .magic = 0x45525054u, .version = 1u, .n_presets = 2u,
    .e = {
        { "Normal",   1, 0, { { PARAM_PITCH, 0x3F800000u } } },  /* 1.0  */
        { "OctaveUp", 1, 0, { { PARAM_PITCH, 0x40000000u } } },  /* 2.0  */
    },
};
const unsigned char *plugin_preset_blob(unsigned int *len)
{
    if (len) *len = (unsigned int)sizeof(g_presets);
    return (const unsigned char *)&g_presets;
}

uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }

/* Keep the sampler ring topped up from the looped bundled sample. */
static void refill(void)
{
    uint32_t room = tessera_sampler_headroom(&g_samp);
    while (room > 0) {
        uint32_t take = room < 64u ? room : 64u;   /* small bounded chunks */
        float chunk[64];
        for (uint32_t i = 0; i < take; i++) {
            chunk[i] = g_sample[g_src_pos];
            g_src_pos = (g_src_pos + 1u) % SAMPLE_LEN;   /* loop the sample */
        }
        tessera_sampler_push(&g_samp, chunk, take);
        room -= take;
    }
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate; (void)block_size;   /* the bundled sample is rate-agnostic */
    /* Build the bundled sample: a decaying sine over the buffer. */
    tessera_osc_t o; tessera_osc_set(&o, (float)SAMPLE_LEN, 2.0f);   /* 2 cycles */
    for (int i = 0; i < SAMPLE_LEN; i++) {
        float env = 1.0f - (float)i / (float)SAMPLE_LEN;             /* linear decay */
        g_sample[i] = tessera_osc_sin(&o) * env;
    }
    g_src_pos = 0;
    g_gate = 1;
    tessera_sampler_init(&g_samp, g_ring, RING_CAP);
    tessera_sampler_set_pitch(&g_samp, 1.0f);
    refill();
    return TESSERA_PLUGIN_OK;
}

void plugin_set_param(uint32_t id, float value)
{
    switch (id) {
    case PARAM_PITCH: tessera_sampler_set_pitch(&g_samp, value); break;
    case PARAM_GATE:  g_gate = value >= 0.5f; break;
    default: break;
    }
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    (void)in_l; (void)in_r;
    refill();
    for (uint32_t i = 0; i < n_frames; i++) {
        float v = g_gate ? tessera_sampler_process(&g_samp) : 0.0f;
        out_l[i] = v;
        out_r[i] = v;
    }
}

void plugin_destroy(void) { }
