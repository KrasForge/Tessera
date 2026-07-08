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

/* Fast, libm-free 2^x and log2(x) (< 0.1% and < 1e-5 error).  Used across the DSP
 * blocks for dB gains (10^(dB/20) = 2^(dB*0.16610)), note-to-frequency, and any
 * exponential/log parameter mapping. */
float tessera_exp2f(float x);
float tessera_log2f(float x);

/* Fast, libm-free sqrt (bit-trick seed + two Newton steps, ~1e-6 relative),
 * atan2 (odd polynomial + octant fixup, < 1e-4 rad), and wrap-to-(-pi, pi].
 * The magnitude/phase toolkit for spectral code. */
float tessera_sqrtf(float x);
float tessera_atan2f(float y, float x);
float tessera_wrap_pi(float p);

/* Tau (2*pi) and pi for phase math. */
#define TESSERA_TAU 6.28318530717958647692f
#define TESSERA_PI  3.14159265358979323846f

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

/* Event kinds (MIDI-shaped).  NOTE_ON/OFF/CC are v1.1; the per-note expression
 * kinds (v1.2, issue #171) carry MPE / MIDI-2.0 style per-note pitch, pressure,
 * and timbre so a synth can voice each note independently. */
#define TESSERA_EV_NOTE_ON  1u
#define TESSERA_EV_NOTE_OFF 2u
#define TESSERA_EV_CC       3u
#define TESSERA_EV_PITCH    4u   /* per-note pitch bend: value = bend, -8192..+8191 */
#define TESSERA_EV_PRESSURE 5u   /* per-note pressure:   data2 = 0..127             */
#define TESSERA_EV_TIMBRE   6u   /* per-note timbre (CC74): data2 = 0..127          */

typedef struct {
    uint8_t  type;     /* TESSERA_EV_*                                  */
    uint8_t  channel;  /* 0..15                                         */
    uint8_t  data1;    /* note number / CC number                       */
    uint8_t  data2;    /* velocity / CC value / pressure / timbre       */
    int16_t  value;    /* high-res expression (pitch bend); 0 otherwise */
    uint16_t frame_offset; /* v1.3: sample within the block, 0..block-1, at
                            * which the event takes effect (0 = block start).
                            * Occupies the field reserved as _pad through v1.2,
                            * so the struct is unchanged at 8 bytes: a v1.2 host
                            * wrote 0 here, which reads as "block start" -
                            * exactly the old per-block behaviour (issue #199). */
} tessera_note_event_t;

/* ABI invariant: the event stays 8 bytes across the v1.1..v1.3 line, so adding
 * frame_offset over the reserved padding shifted no field and grew no queue. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(tessera_note_event_t) == 8,
               "tessera_note_event_t must remain 8 bytes (frozen event layout)");
#endif

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

/* ---- sample-accurate event delivery (v1.3, issue #199) ------------------- *
 * A plugin that ignores frame_offset and drains the whole queue at the top of
 * process_block still works exactly as before (every event then applies at the
 * block start).  A plugin that wants sample accuracy renders the block in
 * segments split at event boundaries, applying each event at its exact frame.
 * The splitter drives that loop: it drains the queue in order, keeping a
 * one-event lookahead, and hands back the [start, start+len) segment to render
 * before the boundary event (if any) is applied.  The host enqueues a block's
 * events in ascending frame_offset order; an out-of-order or out-of-range
 * offset is clamped into [cursor, block] so the loop is always safe and
 * terminating.  No libc, no allocation, wait-free.
 *
 *   tessera_event_split_t sp;
 *   tessera_event_split_init(&sp, TESSERA_EVENT_QUEUE, block);
 *   uint32_t start, len;  tessera_note_event_t ev;  int have;
 *   while (tessera_event_split_next(&sp, &start, &len, &ev, &have)) {
 *       render(voice, outL + start, outR + start, len);  // len may be 0
 *       if (have) apply(voice, &ev);                      // fires at start+len
 *   }
 */
typedef struct {
    tessera_event_queue_t *q;
    uint32_t block;        /* block size in frames                         */
    uint32_t cursor;       /* next unrendered frame                        */
    tessera_note_event_t look;  /* one drained-but-unapplied lookahead event */
    int      have_look;    /* look holds a valid event                     */
    int      done;         /* the block has been fully rendered            */
} tessera_event_split_t;

/* Begin splitting `q`'s events across a block of `block` frames. */
void tessera_event_split_init(tessera_event_split_t *sp,
                              tessera_event_queue_t *q, uint32_t block);

/* Yield the next render segment.  On return 1: render frames
 * [*start, *start + *len) with the current voice state (len may be 0 for
 * back-to-back events at one frame), then if *have_event is non-zero apply
 * *ev - it takes effect at frame *start + *len.  Returns 0 once the block is
 * fully rendered. */
int tessera_event_split_next(tessera_event_split_t *sp,
                             uint32_t *start, uint32_t *len,
                             tessera_note_event_t *ev, int *have_event);

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

/* ---- wavetable oscillator (Theme M15, issue #164) ------------------------ *
 * Plays a single-cycle waveform table with linear interpolation.  To keep it
 * free of gross aliasing across the range, the caller supplies a *mip stack*:
 * one band-limited table per octave above `base_hz`, each with fewer harmonics
 * than the last, and the oscillator selects the table safe for the current
 * frequency.  A single-table stack (n_tables == 1) is the naive, non-band-limited
 * case.  tessera_wt_bandlimit fills one table with a band-limited sawtooth of
 * `n_harmonics`, so a caller can build a stack without shipping raw tables. */
typedef struct {
    const float *const *tables;   /* n_tables single-cycle tables of table_len */
    int      n_tables;
    int      table_len;
    float    base_hz;             /* fundamental the lowest table is built for  */
    float    sr;
    float    phase, inc;
    int      sel;                 /* currently selected table index             */
} tessera_wavetable_t;
void  tessera_wt_init (tessera_wavetable_t *wt, const float *const *tables,
                       int n_tables, int table_len, float base_hz, float sr);
void  tessera_wt_set_freq(tessera_wavetable_t *wt, float freq);
float tessera_wt_process(tessera_wavetable_t *wt);
/* Fill `table[len]` with a band-limited sawtooth using the first `n_harmonics`
 * harmonics (sum of sin(k*phase)/k), normalised to roughly [-1, 1]. */
void  tessera_wt_bandlimit(float *table, int len, int n_harmonics);

/* ---- streaming sampler (Theme M15, issue #165) --------------------------- *
 * Plays a PCM sample that is streamed in from storage rather than held whole in
 * RAM.  The host pushes source samples into a fixed ring (`tessera_sampler_push`)
 * off the audio path; the audio path pulls resampled output
 * (`tessera_sampler_process`) at a pitch ratio with linear interpolation.  The
 * ring is bounded, so memory never grows with the sample length - a sampler
 * cannot blow its per-plugin memory quota no matter how long the sample.  If the
 * refill falls behind, the pull returns silence (and holds position) instead of
 * stalling the audio path.
 *
 * Looping is a fetch-side concern: the host feeds a monotonic stream, re-reading
 * the loop region when it reaches the loop end, so the sampler stays a pure,
 * seamless streaming resampler.  The caller owns the ring buffer. */
typedef struct {
    float   *buf;
    uint32_t cap;        /* ring capacity in samples (the memory bound) */
    uint64_t filled;     /* total samples pushed (monotonic stream length) */
    uint64_t play;       /* Q32 fractional play cursor into the stream */
    uint64_t pitch;      /* Q32 stream samples per output sample (1<<32 = unity) */
} tessera_sampler_t;

/* Initialise over a caller ring of `cap` samples, at unity pitch. */
void  tessera_sampler_init(tessera_sampler_t *s, float *buf, uint32_t cap);
/* Playback speed: 1.0 = original pitch, 2.0 = octave up, 0.5 = octave down. */
void  tessera_sampler_set_pitch(tessera_sampler_t *s, float ratio);
/* Append `n` source samples to the stream (host side, off the audio path).  The
 * ring keeps only the most recent `cap` samples. */
void  tessera_sampler_push(tessera_sampler_t *s, const float *src, uint32_t n);
/* How many more source samples the host may push before the ring is full ahead
 * of the play cursor (so it can size its next read). */
uint32_t tessera_sampler_headroom(const tessera_sampler_t *s);
/* Produce one output sample at the current pitch; returns 0 (silence) without
 * advancing if the stream has not been buffered far enough (underrun). */
float tessera_sampler_process(tessera_sampler_t *s);
/* The integer stream index the play cursor is currently at (for the host's
 * fetch/loop bookkeeping). */
uint64_t tessera_sampler_pos(const tessera_sampler_t *s);

/* ---- FM operators (Theme M15, issue #164) -------------------------------- *
 * A phase-modulatable sine operator: tessera_fm_op_process adds `phase_mod`
 * (in cycles) to the operator's phase before the sine, so operators can modulate
 * each other.  tessera_fm2 is the classic two-operator voice: the modulator's
 * output, scaled by `index`, phase-modulates the carrier. */
typedef struct { float phase, inc; } tessera_fm_op_t;
void  tessera_fm_op_set    (tessera_fm_op_t *op, float sr, float freq);
float tessera_fm_op_process(tessera_fm_op_t *op, float phase_mod);
float tessera_fm2(tessera_fm_op_t *carrier, tessera_fm_op_t *mod, float index);

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

/* ---- reference effects suite (libtessera.a, Theme B, issue #111) --------- *
 * Complete effects composed from the primitives above, so a plugin author can
 * drop in an overdrive, compressor, EQ, delay, chorus, reverb, gate, or tuner
 * without wiring the DSP by hand.  Every block is real-time safe: no libc, no
 * allocation, no unbounded per-sample work.  Delay-based effects take a
 * caller-supplied backing buffer - the SDK never allocates. */

/* Overdrive / distortion: a stateless soft-clipping waveshaper.  `drive` (>= 1)
 * scales the input into the nonlinearity; `level` scales the result.  A rational
 * tanh approximation keeps it smooth and bounded to (-level, +level). */
float tessera_fx_overdrive(float x, float drive, float level);

/* Feed-forward compressor: a peak detector drives a static gain computer
 * (threshold, ratio, makeup).  attack/release are the detector time constants. */
typedef struct {
    tessera_envfollow_t det;
    float thresh_lin, inv_ratio_minus_1, makeup_lin;
} tessera_fx_comp_t;
void  tessera_fx_comp_init(tessera_fx_comp_t *c, float sr, float atk_ms,
                           float rel_ms, float thresh_db, float ratio,
                           float makeup_db);
float tessera_fx_comp(tessera_fx_comp_t *c, float x);

/* 3-band EQ: a low shelf, a peaking mid, and a high shelf in series. */
typedef struct { tessera_biquad_t low, mid, high; } tessera_fx_eq3_t;
void  tessera_fx_eq3_init(tessera_fx_eq3_t *eq, float sr,
                          float low_f,  float low_db,
                          float mid_f,  float mid_q, float mid_db,
                          float high_f, float high_db);
float tessera_fx_eq3(tessera_fx_eq3_t *eq, float x);

/* Delay with feedback and a wet/dry mix.  Set the delay in samples (a host can
 * derive it from tempo with tessera-transport / tempo_sync).  `feedback` in
 * [0,1); `mix` in [0,1] (0 = dry, 1 = wet). */
typedef struct {
    tessera_delay_t line;
    float delay_samples, feedback, mix;
} tessera_fx_delay_t;
void  tessera_fx_delay_init(tessera_fx_delay_t *d, float *buf, uint32_t size);
void  tessera_fx_delay_set (tessera_fx_delay_t *d, float delay_samples,
                            float feedback, float mix);
float tessera_fx_delay(tessera_fx_delay_t *d, float x);

/* Chorus: a short LFO-modulated delay mixed with the dry signal. */
typedef struct {
    tessera_delay_t line;
    tessera_osc_t   lfo;
    float base_samples, depth_samples, mix;
} tessera_fx_chorus_t;
void  tessera_fx_chorus_init(tessera_fx_chorus_t *c, float *buf, uint32_t size,
                             float sr, float rate_hz, float base_ms,
                             float depth_ms, float mix);
float tessera_fx_chorus(tessera_fx_chorus_t *c, float x);

/* Noise gate: below the threshold the signal is smoothly muted; above it, it
 * passes.  attack/release come from the internal smoother and detector. */
typedef struct {
    tessera_envfollow_t det;
    tessera_smooth_t    gain;
    float thresh_lin;
} tessera_fx_gate_t;
void  tessera_fx_gate_init(tessera_fx_gate_t *g, float sr, float thresh_db,
                           float atk_ms, float rel_ms);
float tessera_fx_gate(tessera_fx_gate_t *g, float x);

/* Schroeder reverb: 4 parallel damped feedback combs into 2 series allpasses.
 * The caller supplies 4 comb buffers and 2 allpass buffers; each line's delay is
 * its buffer length, so buffer sizes set the room character.  `feedback` in
 * [0,1) sets the tail length, `damp` in [0,1] the high-frequency decay. */
typedef struct {
    tessera_delay_t comb[4];
    float           comb_lp[4];
    tessera_delay_t ap[2];
    float feedback, damp, mix;
} tessera_fx_reverb_t;
void  tessera_fx_reverb_init(tessera_fx_reverb_t *r,
                             float *comb_buf[4], uint32_t comb_size[4],
                             float *ap_buf[2],   uint32_t ap_size[2],
                             float feedback, float damp, float mix);
float tessera_fx_reverb(tessera_fx_reverb_t *r, float x);

/* ---- polyphonic synth voice engine (libtessera.a, Theme B, issue #113) --- *
 * The ABI (v1.1) delivers note events into a plugin; this turns them into audio
 * - a voice allocator over the SDK's oscillators and ADSR that proves the
 * synth-voice path end to end.  Real-time safe: no libc, no allocation, and
 * per-sample cost bounded by the (fixed) voice count.  The caller owns the voice
 * array, so polyphony is chosen at the call site. */
typedef enum {
    TESSERA_WAVE_SINE = 0, TESSERA_WAVE_SAW, TESSERA_WAVE_SQUARE, TESSERA_WAVE_TRIANGLE,
    TESSERA_WAVE_FM     /* two-operator FM (ratio/index set by tessera_synth_set_fm) */
} tessera_wave_t;

typedef struct {
    tessera_osc_t   osc;
    tessera_fm_op_t fm_car, fm_mod;   /* carrier + modulator for TESSERA_WAVE_FM */
    tessera_adsr_t  adsr;
    int      active;   /* 1 while sounding (through release)  */
    int      note;     /* MIDI note, or -1 when free          */
    float    gain;     /* velocity / 127                      */
    float    bend_semi;/* per-note pitch bend, semitones (MPE, #171) */
    float    pressure; /* per-note pressure, 0..1 (default 1) */
    uint32_t born;     /* allocation order, for voice stealing*/
} tessera_voice_t;

typedef struct {
    tessera_voice_t *voices;
    int      n_voices;
    float    sr;
    tessera_wave_t waveform;
    float    a_ms, d_ms, sustain, r_ms;   /* current patch envelope   */
    float    fm_ratio, fm_index;           /* TESSERA_WAVE_FM settings */
    float    bend_range;                    /* MPE bend range, semitones (default 48) */
    uint32_t age;                          /* monotonic alloc counter  */
} tessera_synth_t;

/* MIDI note -> frequency in Hz (equal temperament, A4 = note 69 = 440 Hz). */
float tessera_note_to_hz(int note);

/* Initialise a synth over a caller-supplied voice array (default sine, a mild
 * envelope).  n_voices sets the polyphony. */
void  tessera_synth_init(tessera_synth_t *s, tessera_voice_t *voices,
                         int n_voices, float sr);
/* Choose the waveform and ADSR patch applied to newly-triggered notes. */
void  tessera_synth_set (tessera_synth_t *s, tessera_wave_t waveform,
                         float a_ms, float d_ms, float sustain, float r_ms);
/* Set the FM modulator ratio (modulator freq / note freq) and index (modulation
 * depth) used by TESSERA_WAVE_FM voices. */
void  tessera_synth_set_fm(tessera_synth_t *s, float ratio, float index);
/* Set the per-note pitch-bend range in semitones for MPE PITCH events
 * (default 48, the MPE convention). */
void  tessera_synth_set_bend_range(tessera_synth_t *s, float semitones);

/* ---- MPE / per-note expression decoder (Theme M17, issue #171) ------------ *
 * Turns a raw MIDI channel-message stream into per-note expression events for
 * the synth.  Under MPE each sounding note gets its own channel, so the
 * channel's pitch bend, channel pressure, and CC 74 (timbre) apply to *that*
 * note; the decoder tracks the active note per channel and tags the events with
 * it.  Feed it one channel message at a time; it emits 0+ tessera events. */
typedef struct {
    int8_t active_note[16];   /* MIDI note sounding on each channel, or -1 */
} tessera_mpe_t;
void tessera_mpe_init(tessera_mpe_t *m);
/* Decode one MIDI channel message (`status`, `d1`, `d2`) into up to `max`
 * tessera note events written to `out`.  Returns the number produced. */
int  tessera_mpe_feed(tessera_mpe_t *m, uint8_t status, uint8_t d1, uint8_t d2,
                      tessera_note_event_t *out, int max);
/* Note on (velocity 0 is treated as note-off); allocates a free voice or steals
 * the quietest.  Note off releases every voice sounding that note. */
void  tessera_synth_note_on (tessera_synth_t *s, int note, int velocity);
void  tessera_synth_note_off(tessera_synth_t *s, int note);
/* Apply a decoded ABI note event (NOTE_ON / NOTE_OFF; others ignored). */
void  tessera_synth_event(tessera_synth_t *s, const tessera_note_event_t *ev);
/* Render one summed sample across all voices; reclaims voices whose release has
 * completed.  The caller scales/limits the mix for its output. */
float tessera_synth_render(tessera_synth_t *s);
/* Count of currently-sounding voices (for meters / tests). */
int   tessera_synth_active(const tessera_synth_t *s);

/* ---- IR convolution (libtessera.a, Theme B, issue #112) ------------------ *
 * A time-domain FIR: convolve the signal against an impulse response (a guitar
 * cabinet, a room, a filter kernel).  Deliberately the heavy building block - a
 * few-thousand-tap IR costs real CPU per block, exercising the M12 budget and
 * the process isolation under load.  Real-time safe: exactly ir_len
 * multiply-adds per sample, and the caller owns both the IR and the history ring
 * (the SDK never allocates).  hist_len must be >= ir_len. */
typedef struct {
    const float *ir;
    float       *hist;
    uint32_t     ir_len, hist_len, w;
} tessera_conv_t;
void  tessera_conv_init (tessera_conv_t *c, const float *ir, uint32_t ir_len,
                         float *hist, uint32_t hist_len);
float tessera_conv      (tessera_conv_t *c, float x);
void  tessera_conv_reset(tessera_conv_t *c);
/* Worst-case (DC) gain of an IR - sum of |taps| - for pre-scaling to avoid
 * clipping. */
float tessera_conv_normgain(const float *ir, uint32_t ir_len);

/* Tuner: estimate the fundamental of a block by interpolated upward
 * zero-crossings.  Call _process with each block, then _hz for the current
 * estimate (0 if too few crossings).  tessera_fx_note_of maps a frequency to
 * the nearest MIDI note, writing the cents offset (-50..+50) if `cents` != NULL. */
typedef struct { float sr, hz; } tessera_fx_tuner_t;
void  tessera_fx_tuner_init   (tessera_fx_tuner_t *t, float sr);
void  tessera_fx_tuner_process(tessera_fx_tuner_t *t, const float *x, uint32_t n);
float tessera_fx_tuner_hz     (const tessera_fx_tuner_t *t);
int   tessera_fx_note_of      (float hz, float *cents);

/* ---- FFT primitive (libtessera.a, Theme M18, issue #184) ------------------ *
 * Radix-2 complex FFT/inverse, in place, power-of-two sizes, plus the packed
 * real pair (audio is real) and the STFT window helpers.  Allocation-free: the
 * caller owns the twiddle table and every buffer.  Twiddles are generated once
 * at setup (off the audio path); the transforms do bounded per-call work.  No
 * libc, no libm. */

typedef struct { float re, im; } tessera_cpx_t;

/* Fill tw[0 .. n/2-1] with e^{-i 2 pi k / n}.  One table serves tessera_fft/
 * tessera_ifft at size n AND tessera_rfft/tessera_irfft over n real samples. */
void tessera_fft_twiddles(tessera_cpx_t *tw, uint32_t n);

/* In-place complex transforms of n points (n a power of two >= 2).  The
 * inverse includes the 1/n scale, so ifft(fft(x)) == x. */
void tessera_fft (tessera_cpx_t *x, const tessera_cpx_t *tw, uint32_t n);
void tessera_ifft(tessera_cpx_t *x, const tessera_cpx_t *tw, uint32_t n);

/* Real transforms of n samples (n a power of two >= 4), via one n/2-point
 * complex FFT.  tessera_rfft writes bins out[0 .. n/2] (DC..Nyquist; both have
 * zero imaginary part), so `out` must hold n/2+1 entries.  tessera_irfft is
 * its inverse (writes n samples to `out`) and uses `in` as scratch - the bins
 * are destroyed. */
void tessera_rfft (const float *in, tessera_cpx_t *out,
                   const tessera_cpx_t *tw, uint32_t n);
void tessera_irfft(tessera_cpx_t *in, float *out,
                   const tessera_cpx_t *tw, uint32_t n);

/* Periodic (DFT-even) windows for STFT analysis: Hann sums to unity at 50%
 * overlap (and Hann^2 at 75%), which is what overlap-add resynthesis needs. */
void tessera_window_hann   (float *w, uint32_t n);
void tessera_window_hamming(float *w, uint32_t n);

/* ---- partitioned FFT convolution (libtessera.a, Theme M18, issue #185) ---- *
 * Long-IR convolution the way real products do it: the IR is split into
 * block-sized partitions transformed once at load, and each block costs one
 * forward FFT, P bin-wise complex MACs, and one inverse FFT - bounded,
 * position-independent work (uniform-partitioned overlap-save with a
 * frequency-domain delay line).  Use tessera_conv_* for short IRs; use this
 * for cabinets and rooms.  Zero added latency: block n's output depends on
 * input up to block n, exactly like the direct engine.
 *
 * The caller owns every buffer.  For block size B (power of two >= 4) and an
 * ir_len-tap IR, with P = tessera_pconv_parts(B, ir_len) and K = B+1 bins:
 *   tw     - B entries,      tessera_fft_twiddles(tw, 2*B)
 *   h_spec - P*K entries     (IR partition spectra, filled by init)
 *   x_spec - P*K entries     (input-spectrum delay line)
 *   acc    - K entries       (per-block accumulator)
 *   work   - 4*B floats      (persistent input frame + inverse scratch)
 * init transforms the IR (call at load time, off the audio path) and returns
 * 0, or -1 on a bad size/NULL.  process consumes and produces exactly B
 * samples. */
typedef struct {
    uint32_t block, fft, bins, parts, slot;
    const tessera_cpx_t *tw;
    tessera_cpx_t *h_spec, *x_spec, *acc;
    float *work;
} tessera_pconv_t;

uint32_t tessera_pconv_parts(uint32_t block, uint32_t ir_len);
int  tessera_pconv_init(tessera_pconv_t *pc, uint32_t block,
                        const float *ir, uint32_t ir_len,
                        const tessera_cpx_t *tw,
                        tessera_cpx_t *h_spec, tessera_cpx_t *x_spec,
                        tessera_cpx_t *acc, float *work);
void tessera_pconv_process(tessera_pconv_t *pc, const float *in, float *out);
void tessera_pconv_reset(tessera_pconv_t *pc);

/* ---- phase vocoder: time-stretch and pitch-shift (Theme M18, issue #186) -- *
 * STFT analysis/synthesis over the FFT primitive: Hann analysis + synthesis
 * windows at 75% overlap, per-bin phase propagation by instantaneous
 * frequency.  The synthesis hop is fixed at n/4 (COLA-exact for Hann^2); the
 * analysis hop is round(hs / ratio), so the effective stretch is the exact
 * rational hs/ha.  Unity ratio reproduces the input (minus the n-sample
 * framework latency).  Allocation-free: the caller owns the twiddles (from
 * tessera_fft_twiddles(tw, n)), a float arena of tessera_pvoc_floats(n)
 * entries, and a bin arena of tessera_pvoc_cpx(n) entries.  No libm.
 *
 * tessera_pvoc_process is hop-granular: it consumes exactly pv.ha input
 * samples and produces exactly pv.hs output samples, time-stretched. */
typedef struct {
    uint32_t n, ha, hs, primed;
    const tessera_cpx_t *tw;
    float *win, *in_ring, *ola, *frame;
    float *prev_phase, *synth_phase, *aphase, *peaks;
    tessera_cpx_t *spec;
} tessera_pvoc_t;

uint32_t tessera_pvoc_floats(uint32_t n);
uint32_t tessera_pvoc_cpx(uint32_t n);
int  tessera_pvoc_init(tessera_pvoc_t *pv, uint32_t n, float ratio,
                       const tessera_cpx_t *tw, float *mem, tessera_cpx_t *cmem);
void tessera_pvoc_process(tessera_pvoc_t *pv, const float *in, float *out);
void tessera_pvoc_reset(tessera_pvoc_t *pv);

/* Pitch shifter: time-stretch by `ratio` then resample back to the original
 * duration, scaling every frequency by `ratio` (2.0 = +1 octave, 0.5 = -1;
 * ratio = 2^(semitones/12)).  Block-size agnostic streaming: each call
 * consumes and produces exactly `count` samples (silence while the STFT
 * pipeline primes).  Float arena: tessera_pshift_floats(n); bins as above. */
typedef struct {
    tessera_pvoc_t pv;
    float ratio, rpos;
    float *in_fifo, *st_fifo, *out_fifo;
    uint32_t cap, in_n, st_n, out_n;
} tessera_pshift_t;

uint32_t tessera_pshift_floats(uint32_t n);
int  tessera_pshift_init(tessera_pshift_t *ps, uint32_t n, float ratio,
                         const tessera_cpx_t *tw, float *mem, tessera_cpx_t *cmem);
void tessera_pshift_process(tessera_pshift_t *ps, const float *in, float *out,
                            uint32_t count);
void tessera_pshift_reset(tessera_pshift_t *ps);

/* ---- spectrum analyser + FFT tuner (Theme M18, issue #187) ---------------- *
 * Analysis for the display: log-frequency spectrum bars and a robust tuner.
 * Both emit the integer values the OLED UI model renders (per-mille levels;
 * pair the tuner with tessera_fx_note_of for note/cents), keeping the kernel
 * side integer-only.  Caller-owned buffers, no allocation, no libm.
 *
 * Spectrum: feed n-sample frames; bars[i] is the bar's current level and
 * peaks[i] its held peak (0..1000 per-mille of the -60..0 dBFS range), over
 * `nbars` log-spaced bands from ~50 Hz to Nyquist.  `decay` is the per-update
 * peak fall, in per-mille.  Arenas: tessera_spectrum_floats(n) floats,
 * tessera_pvoc_cpx(n) bins, tessera_spectrum_u32(nbars) uint32s. */
typedef struct {
    uint32_t n, nbars, decay;
    float sr;
    const tessera_cpx_t *tw;
    float *win, *frame;
    tessera_cpx_t *spec;
    uint32_t *edges, *bars, *peaks;
} tessera_spectrum_t;

uint32_t tessera_spectrum_floats(uint32_t n);
uint32_t tessera_spectrum_u32(uint32_t nbars);
int  tessera_spectrum_init(tessera_spectrum_t *sp, uint32_t n, float sr,
                           uint32_t nbars, uint32_t decay,
                           const tessera_cpx_t *tw,
                           float *mem, tessera_cpx_t *cmem, uint32_t *umem);
void tessera_spectrum_process(tessera_spectrum_t *sp, const float *x);

/* FFT tuner: streaming (any block size); analyses every n/4 samples.  The
 * fundamental is the strongest spectral peak - parabolic interpolation for
 * the first estimate, then refined by the bin's phase advance across the hop
 * (instantaneous frequency), resolving a small fraction of a bin.  Robust
 * under broadband noise, which spreads across bins while the tone stays
 * concentrated.  Returns 0 Hz when no clear tone stands above the noise.
 * Arena: tessera_ftuner_floats(n) floats + tessera_pvoc_cpx(n) bins. */
typedef struct {
    uint32_t n, hop, wpos, fill, primed;
    float sr, hz;
    const tessera_cpx_t *tw;
    float *win, *ring, *frame, *phase;
    tessera_cpx_t *spec;
} tessera_ftuner_t;

uint32_t tessera_ftuner_floats(uint32_t n);
int   tessera_ftuner_init(tessera_ftuner_t *t, uint32_t n, float sr,
                          const tessera_cpx_t *tw, float *mem, tessera_cpx_t *cmem);
void  tessera_ftuner_process(tessera_ftuner_t *t, const float *x, uint32_t count);
float tessera_ftuner_hz(const tessera_ftuner_t *t);
void  tessera_ftuner_reset(tessera_ftuner_t *t);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_SDK_H */
