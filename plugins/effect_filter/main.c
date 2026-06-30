/* plugins/effect_filter/main.c - reference low-pass filter plugin
 *                                 (Issue #29, M6)
 *
 * A worked example of an isolated effect plugin: a resonant low-pass filter
 * built as a Chamberlin state-variable filter (a simple 2-pole IIR).  It is
 * real-time-safe - process_block does only multiplies and adds on static
 * state, with no allocation, no locks, and no syscalls - and exposes two
 * real-time-safe controls:
 *
 *   plugin_set_param(0, cutoff_hz)   - corner frequency
 *   plugin_set_param(1, resonance_q) - resonance (Q)
 *
 * It is written against only <plugin_abi.h>, so it builds with a stock aarch64
 * toolchain and contains no imports from the kernel.
 *
 * The SVF recurrence (per sample, per channel):
 *     low  += f * band;
 *     high  = in - low - q * band;
 *     band += f * high;
 *     out   = low;                 (low-pass output)
 *
 * where f = 2*pi*cutoff/samplerate (the standard small-cutoff approximation of
 * 2*sin(pi*cutoff/samplerate), so no transcendental functions are needed) and
 * q = 1/Q is the damping.  f is clamped for stability.
 */

#include "plugin_abi.h"

#define PARAM_CUTOFF    0u
#define PARAM_RESONANCE 1u

#define PI_F      3.14159265358979f
#define F_MAX     1.0f        /* keep the SVF comfortably stable        */
#define F_MIN     0.0001f
#define DEFAULT_CUTOFF 1000.0f
#define DEFAULT_Q      0.707f /* Butterworth-ish, no resonant peak      */

static uint32_t g_sr = 48000u;
static float    g_f  = 0.1309f;          /* frequency coefficient        */
static float    g_q  = 1.0f / DEFAULT_Q; /* damping = 1/Q                */

/* Per-channel filter state (stereo). */
static float g_low[2];
static float g_band[2];

uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void set_cutoff(float cutoff_hz)
{
    float f = 2.0f * PI_F * cutoff_hz / (float)g_sr;
    g_f = clampf(f, F_MIN, F_MAX);
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    if (sample_rate == 0 || block_size == 0)
        return TESSERA_PLUGIN_EINVAL;
    g_sr = sample_rate;
    g_q  = 1.0f / DEFAULT_Q;
    set_cutoff(DEFAULT_CUTOFF);
    g_low[0] = g_low[1] = 0.0f;
    g_band[0] = g_band[1] = 0.0f;
    return TESSERA_PLUGIN_OK;
}

static inline float svf_step(int ch, float in)
{
    float low  = g_low[ch] + g_f * g_band[ch];
    float high = in - low - g_q * g_band[ch];
    float band = g_band[ch] + g_f * high;
    g_low[ch]  = low;
    g_band[ch] = band;
    return low;                          /* low-pass output */
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    for (uint32_t i = 0; i < n_frames; i++) {
        out_l[i] = svf_step(0, in_l[i]);
        out_r[i] = svf_step(1, in_r[i]);
    }
}

void plugin_set_param(uint32_t param_id, float value)
{
    if (param_id == PARAM_CUTOFF) {
        set_cutoff(value);               /* single coefficient update */
    } else if (param_id == PARAM_RESONANCE) {
        float q = value < 0.5f ? 0.5f : value;   /* avoid self-oscillation */
        g_q = 1.0f / q;
    }
    /* Unknown params are ignored.  Note: changing a coefficient does not touch
     * the filter state, so the output stays continuous (no click) apart from
     * the filter's own transient. */
}

void plugin_destroy(void) { }
