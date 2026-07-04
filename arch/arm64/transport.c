/* arch/arm64/transport.c - master musical transport and clock (Theme C, #114) */

#include "transport.h"

uint32_t transport_ticks_per_beat(const transport_t *t)
{
    uint32_t den = t->den ? t->den : 4u;
    return TP_PPQ * 4u / den;             /* quarter=TP_PPQ; eighth=TP_PPQ/2 */
}

void transport_init(transport_t *t, uint32_t sr, uint32_t tempo_mbpm)
{
    t->sr         = sr;
    t->tempo_mbpm = tempo_mbpm ? tempo_mbpm : 120000u;
    t->num        = 4u;
    t->den        = 4u;
    t->playing    = 0u;
    t->bar = t->beat = t->tick = 0u;
    t->tick_rem   = 0u;
    t->clkout_acc = 0u;
    t->clock_out  = 0u;
    t->clocked    = 0u;
}

void transport_set_tempo(transport_t *t, uint32_t tempo_mbpm)
{
    if (tempo_mbpm == 0u) return;
    t->tempo_mbpm = tempo_mbpm;   /* the remainder accumulator stays valid */
}

void transport_set_timesig(transport_t *t, uint32_t num, uint32_t den)
{
    if (num == 0u || den == 0u) return;
    t->num = num;
    t->den = den;
    /* keep the position sane if the beat length shrank */
    uint32_t tpb = transport_ticks_per_beat(t);
    if (t->tick >= tpb) t->tick = tpb - 1u;
    if (t->beat >= t->num) t->beat = 0u;
}

void transport_start(transport_t *t)
{
    t->bar = t->beat = t->tick = 0u;
    t->tick_rem = 0u;
    t->clkout_acc = 0u;
    t->playing = 1u;
}
void transport_continue(transport_t *t) { t->playing = 1u; }
void transport_stop(transport_t *t)     { t->playing = 0u; }

/* Advance the position by `ticks` internal ticks and accumulate clock-out. */
static void advance_ticks(transport_t *t, uint32_t ticks)
{
    uint32_t tpb = transport_ticks_per_beat(t);
    t->tick += ticks;
    while (t->tick >= tpb) {
        t->tick -= tpb;
        t->beat++;
        if (t->beat >= t->num) { t->beat = 0u; t->bar++; }
    }
    /* MIDI clock out: one 0xF8 every TP_PPQ/24 internal ticks. */
    uint32_t per = TP_PPQ / TP_MIDI_PPQN;      /* 96/24 = 4 */
    t->clkout_acc += ticks;
    while (t->clkout_acc >= per) { t->clkout_acc -= per; t->clock_out++; }
}

void transport_advance(transport_t *t, uint32_t n_frames)
{
    if (!t->playing || t->sr == 0u) return;
    /* Exact: ticks = n_frames * TP_PPQ * tempo_mbpm / (60000 * sr), carrying the
     * remainder so nothing drifts and boundaries land precisely. */
    uint64_t denom = (uint64_t)60000u * t->sr;
    uint64_t numer = (uint64_t)n_frames * TP_PPQ * t->tempo_mbpm + t->tick_rem;
    uint32_t whole = (uint32_t)(numer / denom);
    t->tick_rem    = numer % denom;
    if (whole) advance_ticks(t, whole);
}

void transport_midi_clock_in(transport_t *t, uint32_t frames_since_last)
{
    t->clocked = 1u;
    /* tempo from the clock interval: a quarter note is 24 clocks, so
     *   frames/quarter = 24 * frames_since_last
     *   BPM = 60 * sr / frames/quarter ; mbpm = 1000 * BPM */
    if (frames_since_last > 0u && t->sr > 0u) {
        uint64_t fpq = (uint64_t)TP_MIDI_PPQN * frames_since_last;
        transport_set_tempo(t, (uint32_t)(((uint64_t)60000u * t->sr) / fpq));
    }
    /* advance one MIDI tick worth of internal ticks */
    advance_ticks(t, TP_PPQ / TP_MIDI_PPQN);
}

int transport_clock_out(transport_t *t)
{
    if (t->clock_out == 0u) return 0;
    t->clock_out--;
    return 1;
}

void transport_snapshot(const transport_t *t, transport_snapshot_t *out)
{
    out->flags      = t->playing ? TP_PLAYING : 0u;
    out->tempo_mbpm = t->tempo_mbpm;
    out->bar        = t->bar;
    out->beat       = t->beat;
    out->tick       = t->tick;
    out->ppq        = TP_PPQ;
}
