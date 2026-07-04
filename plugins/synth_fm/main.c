/* plugins/synth_fm/main.c - reference FM synth plugin (Theme M15, issue #167)
 *
 * A worked polyphonic synth built entirely on the SDK: note events drive the
 * SDK's polyphonic voice engine (issue #113) in its two-operator FM mode
 * (issue #164).  It proves the synth-voice path end to end - note in, voices
 * out - and ships factory presets embedded in the ELF (issue #127).
 *
 * Control:
 *   - Live builds drain note events from the event queue (ABI v1.1).
 *   - Every build also accepts scripted notes through parameters, so the offline
 *     host (issue #128) can render it deterministically from a param CSV:
 *       param 0 = note-on  (value = MIDI note, velocity 100)
 *       param 1 = note-off (value = MIDI note)
 *       param 2 = FM ratio      param 3 = FM index
 *       param 4/5/6/7 = attack / decay / sustain / release
 *
 * Real-time-safe: process_block only renders voices (no allocation, no locks,
 * no syscalls).  Built against the SDK (tessera.h).
 */

#include "tessera.h"

#define PARAM_NOTE_ON   0u
#define PARAM_NOTE_OFF  1u
#define PARAM_FM_RATIO  2u
#define PARAM_FM_INDEX  3u
#define PARAM_ATTACK    4u
#define PARAM_DECAY     5u
#define PARAM_SUSTAIN   6u
#define PARAM_RELEASE   7u

#define N_VOICES 8

static tessera_voice_t g_voices[N_VOICES];
static tessera_synth_t g_synth;
static float g_a = 3.0f, g_d = 80.0f, g_s = 0.7f, g_r = 200.0f;

/* ---- embedded factory presets (.tessera.presets, issue #127) ------------- *
 * A packed struct laid out exactly as the preset-blob format: magic 'TPRE',
 * version, count, then {name[24], n_params, reserved, {id,value-bits}...}. */
struct __attribute__((packed)) preset_entry {
    char     name[24];
    uint16_t n_params;
    uint16_t reserved;
    struct { uint32_t id, bits; } p[2];
};
struct __attribute__((packed)) preset_blob {
    uint32_t magic;
    uint16_t version;
    uint16_t n_presets;
    struct preset_entry e[2];
};
static const struct preset_blob g_presets
    __attribute__((section(".tessera.presets"), used)) = {
    .magic = 0x45525054u,   /* 'TPRE' little-endian */
    .version = 1u,
    .n_presets = 2u,
    .e = {
        /* "Bell": ratio 3.5, index 5.0 (bright, inharmonic-ish) */
        { "Bell", 2, 0, { { PARAM_FM_RATIO, 0x40600000u }, { PARAM_FM_INDEX, 0x40A00000u } } },
        /* "Bass": ratio 1.0, index 2.0 (fat, harmonic) */
        { "Bass", 2, 0, { { PARAM_FM_RATIO, 0x3F800000u }, { PARAM_FM_INDEX, 0x40000000u } } },
    },
};
/* Accessor so tools/tests can read the embedded blob without the ELF section. */
const unsigned char *plugin_preset_blob(unsigned int *len)
{
    if (len) *len = (unsigned int)sizeof(g_presets);
    return (const unsigned char *)&g_presets;
}

uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)block_size;
    tessera_synth_init(&g_synth, g_voices, N_VOICES, (float)sample_rate);
    tessera_synth_set(&g_synth, TESSERA_WAVE_FM, g_a, g_d, g_s, g_r);
    tessera_synth_set_fm(&g_synth, 1.0f, 2.0f);
    return TESSERA_PLUGIN_OK;
}

void plugin_set_param(uint32_t id, float value)
{
    switch (id) {
    case PARAM_NOTE_ON:   tessera_synth_note_on(&g_synth, (int)value, 100); break;
    case PARAM_NOTE_OFF:  tessera_synth_note_off(&g_synth, (int)value);     break;
    case PARAM_FM_RATIO:  tessera_synth_set_fm(&g_synth, value, g_synth.fm_index); break;
    case PARAM_FM_INDEX:  tessera_synth_set_fm(&g_synth, g_synth.fm_ratio, value); break;
    case PARAM_ATTACK:    g_a = value; tessera_synth_set(&g_synth, TESSERA_WAVE_FM, g_a, g_d, g_s, g_r); break;
    case PARAM_DECAY:     g_d = value; tessera_synth_set(&g_synth, TESSERA_WAVE_FM, g_a, g_d, g_s, g_r); break;
    case PARAM_SUSTAIN:   g_s = value; tessera_synth_set(&g_synth, TESSERA_WAVE_FM, g_a, g_d, g_s, g_r); break;
    case PARAM_RELEASE:   g_r = value; tessera_synth_set(&g_synth, TESSERA_WAVE_FM, g_a, g_d, g_s, g_r); break;
    default: break;
    }
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    (void)in_l; (void)in_r;   /* a synth ignores its audio input */

#ifndef HOSTTEST
    /* Live builds: drain note/CC events delivered by the host this block. */
    tessera_note_event_t ev;
    while (tessera_event_read(TESSERA_EVENT_QUEUE, &ev))
        tessera_synth_event(&g_synth, &ev);
#endif

    for (uint32_t i = 0; i < n_frames; i++) {
        float v = tessera_synth_render(&g_synth) * 0.25f;   /* headroom for polyphony */
        out_l[i] = v;
        out_r[i] = v;
    }
}

void plugin_destroy(void) { }
