/* sdk/examples/sine_plugin/sine_plugin.c - a complete Tessera plugin built with
 * only the SDK (Issue #38).
 *
 * A stereo sine-wave generator.  It shows every part of the plugin ABI and each
 * SDK helper:
 *
 *   - the five required exports, marked with TESSERA_PLUGIN_EXPORT;
 *   - tessera_sinf() to synthesise the waveform in process_block;
 *   - tessera_clampf() to keep the frequency in a sane range;
 *   - tessera_param_queue_read() to receive live frequency changes from the
 *     host (parameter id 0, in Hz), in addition to the plugin_set_param()
 *     callback.
 *
 * It includes no Tessera kernel headers - only <tessera.h> from the SDK.
 */

#include "tessera.h"

#define PARAM_FREQ    0u        /* parameter id 0: frequency in Hz */
#define DEFAULT_FREQ  440.0f

static float g_sample_rate;     /* Hz, fixed after init            */
static float g_phase;           /* current phase, radians          */
static float g_phase_inc;       /* phase step per sample, radians  */

/* Set the oscillator frequency (Hz), clamped to the audible range.  Phase is
 * preserved, so a live change is click-free. */
static void set_frequency(float hz)
{
    hz = tessera_clampf(hz, 1.0f, 20000.0f);
    g_phase_inc = TESSERA_TAU * hz / g_sample_rate;
}

/* plugin_abi_version(): report the ABI this SDK targets. */
TESSERA_DEFINE_ABI_VERSION()

TESSERA_PLUGIN_EXPORT
int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)block_size;
    g_sample_rate = (float)sample_rate;
    g_phase       = 0.0f;
    set_frequency(DEFAULT_FREQ);
    return TESSERA_PLUGIN_OK;
}

TESSERA_PLUGIN_EXPORT
void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    (void)in_l; (void)in_r;   /* a generator ignores its input */

    /* Apply any parameter changes the host queued (id 0 = frequency). */
    uint32_t id;
    float    value;
    while (tessera_param_queue_read(TESSERA_PARAM_QUEUE, &id, &value)) {
        if (id == PARAM_FREQ)
            set_frequency(value);
    }

    for (uint32_t i = 0; i < n_frames; i++) {
        float s = tessera_sinf(g_phase);
        out_l[i] = s;
        out_r[i] = s;
        g_phase += g_phase_inc;
        if (g_phase >= TESSERA_TAU)
            g_phase -= TESSERA_TAU;
    }
}

TESSERA_PLUGIN_EXPORT
void plugin_set_param(uint32_t param_id, float value)
{
    if (param_id == PARAM_FREQ)
        set_frequency(value);
}

TESSERA_PLUGIN_EXPORT
void plugin_destroy(void)
{
}
