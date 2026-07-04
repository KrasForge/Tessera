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

/* Re-point a voice's oscillators at its note plus its current pitch bend. */
static void voice_retune(tessera_synth_t *s, tessera_voice_t *v)
{
    float hz = hz_of((float)v->note + v->bend_semi);
    tessera_osc_set(&v->osc, s->sr, hz);
    tessera_fm_op_set(&v->fm_car, s->sr, hz);
    tessera_fm_op_set(&v->fm_mod, s->sr, hz * s->fm_ratio);
}

static float voice_osc(tessera_synth_t *s, tessera_voice_t *v)
{
    switch (s->waveform) {
    case TESSERA_WAVE_SAW:      return tessera_osc_saw(&v->osc);
    case TESSERA_WAVE_SQUARE:   return tessera_osc_square(&v->osc);
    case TESSERA_WAVE_TRIANGLE: return tessera_osc_triangle(&v->osc);
    case TESSERA_WAVE_FM:       return tessera_fm2(&v->fm_car, &v->fm_mod, s->fm_index);
    case TESSERA_WAVE_SINE:
    default:                    return tessera_osc_sin(&v->osc);
    }
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
    for (int i = 0; i < n_voices; i++) {
        voices[i].active = 0;
        voices[i].note   = -1;
        voices[i].gain   = 0.0f;
        voices[i].bend_semi = 0.0f;
        voices[i].pressure  = 1.0f;
        voices[i].born    = 0;
        tessera_osc_set(&voices[i].osc, sr, 440.0f);
        tessera_adsr_init(&voices[i].adsr, sr, s->a_ms, s->d_ms, s->sustain, s->r_ms);
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
    tessera_voice_t *v = alloc_voice(s);
    v->active = 1;
    v->note   = note;
    v->gain   = (float)velocity / 127.0f;
    v->born   = ++s->age;
    v->bend_semi = 0.0f;
    v->pressure  = 1.0f;
    /* Point the oscillators at the (un-bent) note; arm the FM operators too
     * (used only by TESSERA_WAVE_FM); reset phase so a retrigger starts cleanly. */
    voice_retune(s, v);
    v->fm_car.phase = 0.0f;
    v->fm_mod.phase = 0.0f;
    tessera_adsr_init(&v->adsr, s->sr, s->a_ms, s->d_ms, s->sustain, s->r_ms);
    tessera_adsr_gate(&v->adsr, 1);
}

void tessera_synth_note_off(tessera_synth_t *s, int note)
{
    /* Release every sounding voice on this note (a note may have been retriggered). */
    for (int i = 0; i < s->n_voices; i++) {
        tessera_voice_t *v = &s->voices[i];
        if (v->active && v->note == note)
            tessera_adsr_gate(&v->adsr, 0);
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
        float env = tessera_adsr(&v->adsr);
        if (v->adsr.stage == TESSERA_ADSR_IDLE) { /* release finished */
            v->active = 0;
            v->note   = -1;
            continue;
        }
        mix += voice_osc(s, v) * env * v->gain * v->pressure;
    }
    return mix;
}

int tessera_synth_active(const tessera_synth_t *s)
{
    int n = 0;
    for (int i = 0; i < s->n_voices; i++) if (s->voices[i].active) n++;
    return n;
}
