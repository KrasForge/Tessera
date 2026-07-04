/* arch/arm64/arp.c - arpeggiator / step-sequencer node (Theme C, issue #116) */

#include "arp.h"

void arp_init(arp_t *a, arp_mode_t mode, uint32_t step_ticks, uint8_t vel)
{
    a->n_held     = 0u;
    a->mode       = (uint32_t)mode;
    a->step_ticks = step_ticks ? step_ticks : 1u;
    a->vel        = vel;
    a->enabled    = 1u;
    a->cur_step   = -1;
    a->playing    = -1;
}

void arp_set_mode(arp_t *a, arp_mode_t mode) { a->mode = (uint32_t)mode; }
void arp_set_step(arp_t *a, uint32_t step_ticks) { if (step_ticks) a->step_ticks = step_ticks; }

void arp_note_on(arp_t *a, uint8_t note)
{
    if (note == 0u) return;                    /* 0 is not a valid note here */
    /* insert sorted, dedup */
    for (uint32_t i = 0; i < a->n_held; i++) {
        if (a->held[i] == note) return;
        if (a->held[i] > note) {
            if (a->n_held >= ARP_MAX_NOTES) return;
            for (uint32_t j = a->n_held; j > i; j--) a->held[j] = a->held[j - 1];
            a->held[i] = note; a->n_held++;
            return;
        }
    }
    if (a->n_held >= ARP_MAX_NOTES) return;
    a->held[a->n_held++] = note;
}

void arp_note_off(arp_t *a, uint8_t note)
{
    for (uint32_t i = 0; i < a->n_held; i++) {
        if (a->held[i] == note) {
            for (uint32_t j = i; j + 1 < a->n_held; j++) a->held[j] = a->held[j + 1];
            a->n_held--;
            return;
        }
    }
}

uint32_t arp_step_index(const arp_t *a, int64_t step)
{
    uint32_t n = a->n_held;
    if (n <= 1u) return 0u;
    uint64_t s = (uint64_t)(step < 0 ? -step : step);   /* patterns are periodic */
    switch (a->mode) {
    case ARP_DOWN:
        return (n - 1u) - (uint32_t)(s % n);
    case ARP_UPDOWN: {
        uint32_t period = 2u * n - 2u;                  /* n>=2 here */
        uint32_t pos = (uint32_t)(s % period);
        return pos < n ? pos : (period - pos);
    }
    case ARP_RANDOM: {
        /* deterministic hash of the step (no RNG state, reproducible) */
        uint64_t h = (s + 1u) * 2654435761u;
        h ^= h >> 13;
        return (uint32_t)(h % n);
    }
    case ARP_UP:
    default:
        return (uint32_t)(s % n);
    }
}

int arp_run(arp_t *a, int64_t abs_tick, arp_event_t *out, int max)
{
    int emitted = 0;

    /* disabled or no notes held: silence any sounding note, then idle. */
    if (!a->enabled || a->n_held == 0u) {
        if (a->playing >= 0 && max - emitted >= 1) {
            out[emitted].on = 0u; out[emitted].note = (uint8_t)a->playing; out[emitted].vel = 0u;
            emitted++;
            a->playing = -1;
        }
        a->cur_step = -1;                    /* re-trigger cleanly when notes return */
        return emitted;
    }

    if (abs_tick < 0) abs_tick = 0;
    int64_t step = abs_tick / (int64_t)a->step_ticks;
    if (step == a->cur_step)
        return 0;                            /* still within the current step */
    a->cur_step = step;

    uint32_t idx = arp_step_index(a, step);
    if (idx >= a->n_held) idx = a->n_held - 1u;
    uint8_t note = a->held[idx];

    /* release the previous note, sound the new one */
    if (a->playing >= 0 && max - emitted >= 1) {
        out[emitted].on = 0u; out[emitted].note = (uint8_t)a->playing; out[emitted].vel = 0u;
        emitted++;
    }
    if (max - emitted >= 1) {
        out[emitted].on = 1u; out[emitted].note = note; out[emitted].vel = a->vel;
        emitted++;
        a->playing = (int)note;
    }
    return emitted;
}

void arp_enable(arp_t *a, int on)
{
    a->enabled = on ? 1u : 0u;
}
