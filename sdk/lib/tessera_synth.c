/* sdk/lib/tessera_synth.c - polyphonic synth voice engine for the Tessera SDK
 * (Theme B, issue #113).
 *
 * The plugin ABI (v1.1) delivers note events into a plugin; this is the other
 * half - a voice allocator that turns those note-on/note-off events into audio,
 * proving the synth-voice path end to end.  Each voice is an oscillator through
 * an ADSR (the SDK's own building blocks); the engine allocates a free voice on
 * note-on, gates it off on note-off, steals the quietest voice under
 * over-subscription, and reclaims a voice once its release has finished.
 *
 * Real-time safe: no libc, no allocation, and per-sample work bounded by the
 * (fixed) voice count.  The caller supplies the voice array, so the SDK never
 * allocates and the polyphony is chosen at the call site.
 */

#include "tessera.h"

float tessera_note_to_hz(int note)
{
    /* 440 Hz = MIDI 69 (A4); 12 equal-tempered semitones per octave. */
    return 440.0f * tessera_exp2f((float)(note - 69) / 12.0f);
}

/* Frequency for a (possibly fractional) note, used to apply per-note pitch bend. */
static float hz_of(float note)
{
    return 440.0f * tessera_exp2f((note - 69.0f) / 12.0f);
}

/* Re-point a voice's oscillators at its current pitch: the glide position
 * (pitch_semi, which is just the note in poly mode) plus its per-note bend
 * plus the global mod-matrix pitch offset (#189). */
static void voice_retune(tessera_synth_t *s, tessera_voice_t *v)
{
    float semi = v->pitch_semi + v->bend_semi + s->pitch_mod;
    float hz   = hz_of(semi);
    v->note_hz = hz;
    tessera_osc_set(&v->osc, s->sr, hz);
    tessera_fm_op_set(&v->fm_car, s->sr, hz);
    tessera_fm_op_set(&v->fm_mod, s->sr, hz * s->fm_ratio);
    if (s->uni_n > 1) {
        /* Copies evenly spread across uni_detune cents of total width. */
        for (int u = 0; u < s->uni_n; u++) {
            float off = s->uni_detune *
                        ((float)u / (float)(s->uni_n - 1) - 0.5f);
            tessera_osc_set(&v->uosc[u], s->sr,
                            hz * tessera_exp2f(off / 1200.0f));
        }
    }
}

static float osc_wave(tessera_synth_t *s, tessera_osc_t *o)
{
    switch (s->waveform) {
    case TESSERA_WAVE_SAW:      return tessera_osc_saw(o);
    case TESSERA_WAVE_SQUARE:   return tessera_osc_square(o);
    case TESSERA_WAVE_TRIANGLE: return tessera_osc_triangle(o);
    case TESSERA_WAVE_SINE:
    default:                    return tessera_osc_sin(o);
    }
}

static float voice_osc(tessera_synth_t *s, tessera_voice_t *v)
{
    if (s->waveform == TESSERA_WAVE_FM)
        return tessera_fm2(&v->fm_car, &v->fm_mod, s->fm_index);
    if (s->uni_n > 1) {
        float sum = 0.0f;
        for (int u = 0; u < s->uni_n; u++)
            sum += osc_wave(s, &v->uosc[u]);
        return sum * s->uni_gain;
    }
    return osc_wave(s, &v->osc);
}

void tessera_synth_init(tessera_synth_t *s, tessera_voice_t *voices,
                        int n_voices, float sr)
{
    s->voices   = voices;
    s->n_voices = n_voices;
    s->sr       = sr;
    s->waveform = TESSERA_WAVE_SINE;
    /* A gentle default patch; override with tessera_synth_set. */
    s->a_ms = 5.0f; s->d_ms = 60.0f; s->sustain = 0.7f; s->r_ms = 120.0f;
    s->fm_ratio = 1.0f; s->fm_index = 0.0f;   /* FM off until set */
    s->bend_range = 48.0f;                     /* MPE default +/- 48 semitones */
    s->age  = 0;
    /* Voice architecture (#189): everything off - #113 behaviour exactly. */
    s->flt_on = 0;
    s->flt_cutoff = 1000.0f; s->flt_res = 0.0f;
    s->flt_env = 0.0f; s->flt_track = 0.0f;
    s->fa_ms = 5.0f; s->fd_ms = 100.0f; s->fsustain = 1.0f; s->fr_ms = 100.0f;
    s->uni_n = 1; s->uni_detune = 0.0f; s->uni_gain = 1.0f;
    s->mode = TESSERA_SYNTH_POLY; s->glide_ms = 0.0f;
    s->pitch_mod = 0.0f; s->cutoff_mod = 0.0f; s->amp_mod = 1.0f;
    for (int i = 0; i < n_voices; i++) {
        voices[i].active = 0;
        voices[i].note   = -1;
        voices[i].gain   = 0.0f;
        voices[i].bend_semi = 0.0f;
        voices[i].pressure  = 1.0f;
        voices[i].born    = 0;
        voices[i].pitch_semi = voices[i].target_semi = 69.0f;
        voices[i].glide_step = 0.0f;
        voices[i].note_hz    = 440.0f;
        tessera_osc_set(&voices[i].osc, sr, 440.0f);
        tessera_adsr_init(&voices[i].adsr, sr, s->a_ms, s->d_ms, s->sustain, s->r_ms);
        tessera_adsr_init(&voices[i].fadsr, sr, s->fa_ms, s->fd_ms, s->fsustain, s->fr_ms);
        tessera_svf_init(&voices[i].svf);
    }
}

void tessera_synth_set(tessera_synth_t *s, tessera_wave_t waveform,
                       float a_ms, float d_ms, float sustain, float r_ms)
{
    s->waveform = waveform;
    s->a_ms = a_ms; s->d_ms = d_ms; s->sustain = sustain; s->r_ms = r_ms;
    /* Re-arm every voice's envelope shape; in-flight voices keep their stage. */
    for (int i = 0; i < s->n_voices; i++) {
        int stage = s->voices[i].adsr.stage;
        tessera_adsr_init(&s->voices[i].adsr, s->sr, a_ms, d_ms, sustain, r_ms);
        s->voices[i].adsr.stage = stage;
    }
}

void tessera_synth_set_fm(tessera_synth_t *s, float ratio, float index)
{
    s->fm_ratio = ratio < 0.0f ? 0.0f : ratio;
    s->fm_index = index < 0.0f ? 0.0f : index;
}

/* ---- voice architecture (issue #189) -------------------------------------- */

void tessera_synth_set_filter(tessera_synth_t *s, int on,
                              float cutoff, float res, float env, float track,
                              float a_ms, float d_ms, float sustain, float r_ms)
{
    s->flt_on     = on != 0;
    s->flt_cutoff = cutoff;
    s->flt_res    = tessera_clampf(res, 0.0f, 0.95f);
    s->flt_env    = env;
    s->flt_track  = tessera_clampf(track, 0.0f, 1.0f);
    s->fa_ms = a_ms; s->fd_ms = d_ms;
    s->fsustain = tessera_clampf(sustain, 0.0f, 1.0f); s->fr_ms = r_ms;
    for (int i = 0; i < s->n_voices; i++) {
        int stage = s->voices[i].fadsr.stage;
        tessera_adsr_init(&s->voices[i].fadsr, s->sr, a_ms, d_ms,
                          s->fsustain, r_ms);
        s->voices[i].fadsr.stage = stage;
    }
}

void tessera_synth_set_unison(tessera_synth_t *s, int n, float detune_cents)
{
    if (n < 1) n = 1;
    if (n > TESSERA_UNI_MAX) n = TESSERA_UNI_MAX;
    s->uni_n      = n;
    s->uni_detune = detune_cents < 0.0f ? 0.0f : detune_cents;
    s->uni_gain   = 1.0f / tessera_sqrtf((float)n);
    for (int i = 0; i < s->n_voices; i++)
        if (s->voices[i].active)
            voice_retune(s, &s->voices[i]);
}

void tessera_synth_set_mode(tessera_synth_t *s, int mode, float glide_ms)
{
    s->mode = (mode == TESSERA_SYNTH_MONO || mode == TESSERA_SYNTH_LEGATO)
            ? mode : TESSERA_SYNTH_POLY;
    s->glide_ms = glide_ms < 0.0f ? 0.0f : glide_ms;
}

void tessera_synth_mod(tessera_synth_t *s, float pitch_semi, float cutoff_hz,
                       float amp)
{
    int retune = s->pitch_mod != pitch_semi;
    s->pitch_mod  = pitch_semi;
    s->cutoff_mod = cutoff_hz;
    s->amp_mod    = amp < 0.0f ? 0.0f : amp;
    if (retune)
        for (int i = 0; i < s->n_voices; i++)
            if (s->voices[i].active)
                voice_retune(s, &s->voices[i]);
}

/* Pick a voice for a new note: a free one if any, else steal the quietest
 * (lowest envelope level), breaking ties toward the oldest. */
static tessera_voice_t *alloc_voice(tessera_synth_t *s)
{
    tessera_voice_t *best = &s->voices[0];
    for (int i = 0; i < s->n_voices; i++) {
        tessera_voice_t *v = &s->voices[i];
        if (!v->active) return v;
        if (v->adsr.level < best->adsr.level ||
            (v->adsr.level == best->adsr.level && v->born < best->born))
            best = v;
    }
    return best;
}

void tessera_synth_note_on(tessera_synth_t *s, int note, int velocity)
{
    if (velocity <= 0) { tessera_synth_note_off(s, note); return; }

    tessera_voice_t *v;
    int retrigger = 1;

    if (s->mode == TESSERA_SYNTH_POLY) {
        v = alloc_voice(s);
        v->pitch_semi = v->target_semi = (float)note;
        v->glide_step = 0.0f;
    } else {
        /* MONO / LEGATO: one voice; glide from the current pitch (#189). */
        v = &s->voices[0];
        int sounding = v->active &&
                       v->adsr.stage != TESSERA_ADSR_IDLE &&
                       v->adsr.stage != TESSERA_ADSR_RELEASE;
        retrigger = !(s->mode == TESSERA_SYNTH_LEGATO && sounding);

        v->target_semi = (float)note;
        if (v->active && s->glide_ms > 0.0f &&
            v->pitch_semi != v->target_semi) {
            float samples = s->glide_ms * 0.001f * s->sr;
            if (samples < 1.0f) samples = 1.0f;
            v->glide_step = (v->target_semi - v->pitch_semi) / samples;
        } else {
            v->pitch_semi = v->target_semi;
            v->glide_step = 0.0f;
        }
    }

    v->active = 1;
    v->note   = note;
    v->gain   = (float)velocity / 127.0f;
    v->born   = ++s->age;
    if (retrigger) {
        v->bend_semi = 0.0f;
        v->pressure  = 1.0f;
    }
    /* Point the oscillators at the pitch; arm the FM operators too (used only
     * by TESSERA_WAVE_FM).  A retrigger resets phases so it starts cleanly; a
     * legato continuation keeps them running (no click, no envelope reset). */
    voice_retune(s, v);
    if (retrigger) {
        v->fm_car.phase = 0.0f;
        v->fm_mod.phase = 0.0f;
        tessera_adsr_init(&v->adsr, s->sr, s->a_ms, s->d_ms, s->sustain, s->r_ms);
        tessera_adsr_gate(&v->adsr, 1);
        tessera_adsr_init(&v->fadsr, s->sr, s->fa_ms, s->fd_ms, s->fsustain, s->fr_ms);
        tessera_adsr_gate(&v->fadsr, 1);
        tessera_svf_init(&v->svf);
    }
}

void tessera_synth_note_off(tessera_synth_t *s, int note)
{
    /* Release every sounding voice on this note (a note may have been retriggered). */
    for (int i = 0; i < s->n_voices; i++) {
        tessera_voice_t *v = &s->voices[i];
        if (v->active && v->note == note) {
            tessera_adsr_gate(&v->adsr, 0);
            tessera_adsr_gate(&v->fadsr, 0);
        }
    }
}

void tessera_synth_set_bend_range(tessera_synth_t *s, float semitones)
{
    s->bend_range = semitones < 0.0f ? 0.0f : semitones;
}

/* Apply a per-note expression event to the sounding voice(s) on that note. */
static void apply_expression(tessera_synth_t *s, const tessera_note_event_t *ev)
{
    for (int i = 0; i < s->n_voices; i++) {
        tessera_voice_t *v = &s->voices[i];
        if (!v->active || v->note != ev->data1)
            continue;
        switch (ev->type) {
        case TESSERA_EV_PITCH:
            v->bend_semi = ((float)ev->value / 8192.0f) * s->bend_range;
            voice_retune(s, v);
            break;
        case TESSERA_EV_PRESSURE:
            v->pressure = (float)ev->data2 / 127.0f;
            break;
        default: break;   /* TIMBRE reserved for a future timbral mapping */
        }
    }
}

void tessera_synth_event(tessera_synth_t *s, const tessera_note_event_t *ev)
{
    switch (ev->type) {
    case TESSERA_EV_NOTE_ON:  tessera_synth_note_on(s, ev->data1, ev->data2); break;
    case TESSERA_EV_NOTE_OFF: tessera_synth_note_off(s, ev->data1);           break;
    case TESSERA_EV_PITCH:
    case TESSERA_EV_PRESSURE:
    case TESSERA_EV_TIMBRE:   apply_expression(s, ev);                        break;
    default: break;   /* CC and others are the plugin's business, not the engine's */
    }
}

float tessera_synth_render(tessera_synth_t *s)
{
    float mix = 0.0f;
    for (int i = 0; i < s->n_voices; i++) {
        tessera_voice_t *v = &s->voices[i];
        if (!v->active) continue;

        /* Glide (#189): walk the pitch toward its target, retuning as it
         * moves, so portamento is a continuous ramp rather than steps. */
        if (v->glide_step != 0.0f) {
            v->pitch_semi += v->glide_step;
            if ((v->glide_step > 0.0f && v->pitch_semi >= v->target_semi) ||
                (v->glide_step < 0.0f && v->pitch_semi <= v->target_semi)) {
                v->pitch_semi = v->target_semi;
                v->glide_step = 0.0f;
            }
            voice_retune(s, v);
        }

        float env = tessera_adsr(&v->adsr);
        if (v->adsr.stage == TESSERA_ADSR_IDLE) { /* release finished */
            v->active = 0;
            v->note   = -1;
            continue;
        }

        float sig = voice_osc(s, v);

        /* Per-voice filter (#189): cutoff = base + envelope sweep + the
         * mod-matrix offset, scaled by key tracking (cutoff follows the note
         * relative to middle C). */
        if (s->flt_on) {
            float fenv   = tessera_adsr(&v->fadsr);
            float cutoff = s->flt_cutoff + s->flt_env * fenv + s->cutoff_mod;
            if (s->flt_track > 0.0f)
                cutoff *= tessera_exp2f(s->flt_track *
                                        tessera_log2f(v->note_hz / 261.63f));
            tessera_svf_set(&v->svf, s->sr, cutoff, s->flt_res);
            sig = tessera_svf_low(&v->svf, sig);
        }

        mix += sig * env * v->gain * v->pressure;
    }
    return mix * s->amp_mod;
}

int tessera_synth_active(const tessera_synth_t *s)
{
    int n = 0;
    for (int i = 0; i < s->n_voices; i++) if (s->voices[i].active) n++;
    return n;
}
