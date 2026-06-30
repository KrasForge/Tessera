/* arch/arm64/audio_core.c - dedicated audio-core loop + watchdog (Issue #21) */

#include "audio_core.h"

void audio_wd_init(audio_wd_t *wd, uint64_t budget_cycles)
{
    wd->budget   = budget_cycles;
    wd->count    = 0;
    wd->overruns = 0;
    wd->worst    = 0;
}

int audio_wd_account(audio_wd_t *wd, uint64_t service_cycles)
{
    wd->count++;
    if (service_cycles > wd->worst)
        wd->worst = service_cycles;
    if (service_cycles > wd->budget) {
        wd->overruns++;
        return 1;
    }
    return 0;
}

void audio_core_init(audio_core_t *ac, spsc_ring_t *ring, int16_t *dma_buf,
                     uint32_t frames, uint64_t budget_cycles)
{
    ac->ring     = ring;
    ac->dma_buf  = dma_buf;
    ac->frames   = frames;
    ac->serviced = 0;
    ac->max_wake = 0;
    audio_wd_init(&ac->wd, budget_cycles);
}

uint32_t audio_core_fill(audio_core_t *ac)
{
    uint32_t want = ac->frames * 2u;                 /* interleaved stereo */
    uint32_t got  = spsc_read(ac->ring, ac->dma_buf, want);

    /* Underrun: emit silence for the missing tail rather than replaying stale
     * samples (an audible glitch, but bounded and click-free at zero). */
    for (uint32_t i = got; i < want; i++)
        ac->dma_buf[i] = 0;

    return got;
}

void audio_core_refill(audio_core_t *ac, uint64_t service_cycles)
{
    audio_core_fill(ac);
    audio_wd_account(&ac->wd, service_cycles);
    ac->serviced++;
}
