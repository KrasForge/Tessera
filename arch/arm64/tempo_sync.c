/* arch/arm64/tempo_sync.c - tempo-synced note values and tap tempo (Theme C) */

#include "tempo_sync.h"

uint32_t tempo_sync_samples(uint32_t tempo_mbpm, uint32_t sr, uint32_t mult, uint32_t div)
{
    if (tempo_mbpm == 0u || div == 0u) return 0u;
    /* quarter_samples = sr * 60000 / tempo_mbpm ; * mult / div, kept in 64-bit
     * so the rounding is applied once at the end. */
    uint64_t numer = (uint64_t)sr * 60000u * mult;
    uint64_t denom = (uint64_t)tempo_mbpm * div;
    return (uint32_t)((numer + denom / 2u) / denom);   /* round to nearest */
}

uint32_t tempo_sync_ms(uint32_t tempo_mbpm, uint32_t mult, uint32_t div)
{
    if (tempo_mbpm == 0u || div == 0u) return 0u;
    /* ms per quarter = 60000000 / tempo_mbpm ; * mult / div. */
    uint64_t numer = (uint64_t)60000000u * mult;
    uint64_t denom = (uint64_t)tempo_mbpm * div;
    return (uint32_t)((numer + denom / 2u) / denom);
}

void taptempo_init(taptempo_t *tt)
{
    for (uint32_t i = 0; i < TS_TAPS; i++) tt->interval[i] = 0u;
    tt->n = 0u;
    tt->head = 0u;
    tt->avg = 0u;
    tt->pending = 0u;
}

static int within2x(uint32_t x, uint32_t ref)
{
    return ref != 0u && x <= ref * 2u && x >= ref / 2u;
}

int taptempo_tap(taptempo_t *tt, uint32_t dt)
{
    if (dt == 0u)
        return 0;

    if (tt->n > 0u && !within2x(dt, tt->avg)) {
        /* An outlier versus the current estimate. */
        if (within2x(dt, tt->pending)) {
            /* A second tap consistent with the last outlier: adopt it as a new
             * tempo, seeded from both intervals. */
            uint32_t prev = tt->pending;
            taptempo_init(tt);
            tt->interval[0] = prev;
            tt->head = 1u; tt->n = 1u;
        } else {
            tt->pending = dt;      /* hold it; leave the estimate untouched */
            return 0;
        }
    }

    tt->pending = 0u;
    tt->interval[tt->head] = dt;
    tt->head = (tt->head + 1u) % TS_TAPS;
    if (tt->n < TS_TAPS) tt->n++;

    uint64_t sum = 0;
    for (uint32_t i = 0; i < tt->n; i++) sum += tt->interval[i];
    tt->avg = (uint32_t)(sum / tt->n);
    return 1;
}

uint32_t taptempo_mbpm(const taptempo_t *tt, uint32_t sr)
{
    if (tt->avg == 0u) return 0u;
    /* BPM = 60 * sr / frames_per_beat ; mbpm = 60000 * sr / avg. */
    return (uint32_t)(((uint64_t)60000u * sr + tt->avg / 2u) / tt->avg);
}
