/* sdk/tessera.h - Tessera Plugin SDK, single convenience header (Issue #38)
 *
 * Include this one header to write a Tessera audio plugin.  It wraps the frozen
 * plugin ABI (plugin_abi.h, bundled alongside this file so the SDK is
 * self-contained - it depends on NO Tessera kernel header) and adds the small
 * helpers a plugin author actually reaches for:
 *
 *   - convenience macros: TESSERA_PLUGIN_EXPORT, TESSERA_ABI_VERSION;
 *   - DSP math (libtessera.a): tessera_sinf(), tessera_clampf();
 *   - control input (libtessera.a): tessera_param_queue_read(), to drain the
 *     host's parameter queue from process_block.
 *
 * The full contract is documented in docs/plugin-abi.md.  Build against this
 * header and link libtessera.a with a stock aarch64 bare-metal toolchain; see
 * sdk/Makefile.template and sdk/examples/sine_plugin.
 */

#ifndef TESSERA_SDK_H
#define TESSERA_SDK_H

#include "plugin_abi.h"   /* the frozen ABI: the five exports + version */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- convenience macros -------------------------------------------------- */

/* The ABI version this SDK targets (Plugin ABI v1). */
#define TESSERA_ABI_VERSION TESSERA_PLUGIN_ABI_VERSION

/* Mark an ABI entry point for export: plain C linkage, always emitted, and
 * default (externally visible) so the host can resolve it by name.  Use it on
 * each of the five required functions.  Example:
 *
 *     TESSERA_PLUGIN_EXPORT uint32_t plugin_abi_version(void) {
 *         return TESSERA_ABI_VERSION;
 *     }
 */
#ifdef __cplusplus
#define TESSERA_PLUGIN_EXPORT \
    extern "C" __attribute__((used, visibility("default")))
#else
#define TESSERA_PLUGIN_EXPORT \
    __attribute__((used, visibility("default")))
#endif

/* Convenience: a canonical plugin_abi_version() body. */
#define TESSERA_DEFINE_ABI_VERSION() \
    TESSERA_PLUGIN_EXPORT uint32_t plugin_abi_version(void) { \
        return TESSERA_ABI_VERSION; \
    }

/* ---- DSP math (libtessera.a) --------------------------------------------- */

/* Fast sine approximation.  `x` is in radians (any magnitude; range-reduced
 * internally).  Returns sin(x) with < 0.1% peak error - ample for audio.  No
 * libc, no allocation, real-time safe. */
float tessera_sinf(float x);

/* Clamp `x` to [lo, hi].  Real-time safe. */
float tessera_clampf(float x, float lo, float hi);

/* Tau (2*pi) for phase math. */
#define TESSERA_TAU 6.28318530717958647692f

/* ---- control input: the host parameter queue ----------------------------- */

/* The host delivers parameter changes to a plugin through a small lock-free
 * single-producer/single-consumer queue in a shared page, mapped read/write
 * into the plugin at this fixed virtual address.  A plugin may drain it at the
 * top of process_block with tessera_param_queue_read() (an alternative to, and
 * compatible with, the plugin_set_param() callback).  The float value is
 * carried as its 32-bit bit pattern.  This layout is part of the host contract
 * (see docs/plugin-abi.md); it is NOT a kernel header. */
#define TESSERA_PARAM_QUEUE_VA (0x8000000000ull + 0x0E000000ull)

typedef struct {
    uint32_t id;
    uint32_t bits;      /* float value as a 32-bit bit pattern */
} tessera_param_event_t;

typedef struct {
    uint32_t magic;     /* TESSERA_PARAM_QUEUE_MAGIC */
    uint32_t capacity;  /* events (power of two)     */
    uint32_t mask;
    uint32_t _pad;
    uint32_t head;      /* producer index (release)  */
    uint32_t tail;      /* consumer index (release)  */
    /* tessera_param_event_t events[capacity] follow immediately. */
} tessera_param_queue_t;

#define TESSERA_PARAM_QUEUE_MAGIC 0x51505141u   /* 'AQPQ' */

/* The queue mapped into this plugin, or NULL-safe to pass explicitly. */
#define TESSERA_PARAM_QUEUE \
    ((tessera_param_queue_t *)(uintptr_t)TESSERA_PARAM_QUEUE_VA)

/* Drain one pending (id, value) event from the queue.  Returns 1 and writes
 * *id and *value if an event was available, 0 if the queue was empty.  Uses
 * acquire/release ordering so it is wait-free and real-time safe.  Typical use:
 *
 *     uint32_t id; float v;
 *     while (tessera_param_queue_read(TESSERA_PARAM_QUEUE, &id, &v))
 *         apply_param(id, v);
 */
int tessera_param_queue_read(tessera_param_queue_t *q, uint32_t *id, float *value);

/* ---- note events and transport (Plugin ABI v1.1, issue #124) -------------- *
 * A v1.1 host delivers MIDI/note events and a musical-time snapshot to a plugin
 * through a second lock-free SPSC queue mapped read/write at a fixed VA, exactly
 * like the parameter queue.  A v1.0 plugin never reads it, so v1.1 is a
 * backward-compatible (minor) ABI bump: a v1.0 plugin still loads on a v1.1
 * host, and a v1.1 plugin is refused by a host too old to understand it (see
 * tessera_abi_compatible in plugin_abi.h).  This layout is part of the host
 * contract (docs/plugin-abi.md); it is NOT a kernel header. */
#define TESSERA_EVENT_QUEUE_VA    (0x8000000000ull + 0x0F000000ull)
#define TESSERA_EVENT_QUEUE_MAGIC 0x51565145u   /* 'EQVQ' */

/* Event kinds (MIDI-shaped). */
#define TESSERA_EV_NOTE_ON  1u
#define TESSERA_EV_NOTE_OFF 2u
#define TESSERA_EV_CC       3u

typedef struct {
    uint8_t type;      /* TESSERA_EV_*                      */
    uint8_t channel;   /* 0..15                             */
    uint8_t data1;     /* note number / CC number           */
    uint8_t data2;     /* velocity / CC value               */
} tessera_note_event_t;

/* Transport snapshot for the current block. */
#define TESSERA_TRANSPORT_PLAYING 1u
typedef struct {
    uint32_t flags;      /* TESSERA_TRANSPORT_*                    */
    uint32_t tempo_mbpm; /* tempo in milli-BPM (120000 == 120 BPM)*/
    uint32_t bar;        /* current bar  (0-based)                */
    uint32_t beat;       /* current beat (0-based)                */
    uint32_t tick;       /* ticks into the current beat           */
    uint32_t ppq;        /* ticks per quarter note                */
} tessera_transport_t;

typedef struct {
    uint32_t magic;      /* TESSERA_EVENT_QUEUE_MAGIC             */
    uint32_t capacity;   /* events (power of two)                 */
    uint32_t mask;
    uint32_t _pad;
    uint32_t head;       /* producer (host) index, release        */
    uint32_t tail;       /* consumer (plugin) index, release      */
    tessera_transport_t transport;   /* refreshed by the host each block */
    /* tessera_note_event_t events[capacity] follow immediately. */
} tessera_event_queue_t;

#define TESSERA_EVENT_QUEUE \
    ((tessera_event_queue_t *)(uintptr_t)TESSERA_EVENT_QUEUE_VA)

/* Drain one note/CC event from the queue.  Returns 1 and writes *ev if one was
 * available, 0 if empty.  Wait-free, real-time safe.  Typical use:
 *
 *     tessera_note_event_t ev;
 *     while (tessera_event_read(TESSERA_EVENT_QUEUE, &ev)) handle(&ev);
 */
int  tessera_event_read(tessera_event_queue_t *q, tessera_note_event_t *ev);

/* Copy the current-block transport snapshot into *out (zeroed if unavailable). */
void tessera_transport_read(const tessera_event_queue_t *q, tessera_transport_t *out);

/* ---- DSP building blocks (libtessera.a) ---------------------------------- *
 * Real-time-safe primitives so authors do not start from sinf and a bare
 * buffer: one-pole smoothers, RBJ biquads, oscillators (polyBLEP anti-aliased),
 * a fractional delay line, an envelope follower, and an ADSR.  No libc, no
 * allocation, no unbounded per-sample work. */

/* One-pole parameter smoother (click-free control changes). */
typedef struct { float y, a; } tessera_smooth_t;
void  tessera_smooth_init(tessera_smooth_t *s, float sr, float time_ms);
void  tessera_smooth_set(tessera_smooth_t *s, float value);   /* jump, no glide */
float tessera_smooth(tessera_smooth_t *s, float target);       /* one step toward target */

/* RBJ-cookbook biquad, transposed direct form II.  Design once, then process
 * per sample.  `q` is the quality factor; `gain_db` the shelf/peak gain. */
typedef struct { float b0, b1, b2, a1, a2; float z1, z2; } tessera_biquad_t;
void  tessera_biquad_lowpass  (tessera_biquad_t *bq, float sr, float f, float q);
void  tessera_biquad_highpass (tessera_biquad_t *bq, float sr, float f, float q);
void  tessera_biquad_bandpass (tessera_biquad_t *bq, float sr, float f, float q);
void  tessera_biquad_notch    (tessera_biquad_t *bq, float sr, float f, float q);
void  tessera_biquad_peaking  (tessera_biquad_t *bq, float sr, float f, float q, float gain_db);
void  tessera_biquad_lowshelf (tessera_biquad_t *bq, float sr, float f, float q, float gain_db);
void  tessera_biquad_highshelf(tessera_biquad_t *bq, float sr, float f, float q, float gain_db);
void  tessera_biquad_reset    (tessera_biquad_t *bq);
float tessera_biquad_process  (tessera_biquad_t *bq, float x);

/* Oscillator.  Set frequency, then call one waveform per sample; saw/square use
 * polyBLEP to suppress the worst aliasing. */
typedef struct { float phase, inc; } tessera_osc_t;
void  tessera_osc_set     (tessera_osc_t *o, float sr, float freq);
float tessera_osc_sin     (tessera_osc_t *o);
float tessera_osc_saw     (tessera_osc_t *o);
float tessera_osc_square  (tessera_osc_t *o);
float tessera_osc_triangle(tessera_osc_t *o);

/* Fractional (linearly-interpolated) delay line.  The caller supplies the
 * backing buffer (`size` floats) - the SDK never allocates. */
typedef struct { float *buf; uint32_t size, w; } tessera_delay_t;
void  tessera_delay_init (tessera_delay_t *d, float *buf, uint32_t size);
void  tessera_delay_write(tessera_delay_t *d, float x);
float tessera_delay_read (const tessera_delay_t *d, float delay_samples);
float tessera_delay_tick (tessera_delay_t *d, float x, float delay_samples);

/* Peak envelope follower with attack/release time constants (ms). */
typedef struct { float env, atk, rel; } tessera_envfollow_t;
void  tessera_envfollow_init(tessera_envfollow_t *e, float sr, float atk_ms, float rel_ms);
float tessera_envfollow     (tessera_envfollow_t *e, float x);

/* ADSR envelope generator. */
typedef enum {
    TESSERA_ADSR_IDLE = 0, TESSERA_ADSR_ATTACK, TESSERA_ADSR_DECAY,
    TESSERA_ADSR_SUSTAIN, TESSERA_ADSR_RELEASE
} tessera_adsr_stage_t;
typedef struct { float a_rate, d_rate, r_rate, sustain, level; int stage; } tessera_adsr_t;
void  tessera_adsr_init(tessera_adsr_t *e, float sr, float a_ms, float d_ms,
                        float sustain, float r_ms);
void  tessera_adsr_gate(tessera_adsr_t *e, int on);   /* on = note-on, off = note-off */
float tessera_adsr     (tessera_adsr_t *e);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_SDK_H */
