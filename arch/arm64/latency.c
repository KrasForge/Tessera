/* arch/arm64/latency.c - audio-callback latency/jitter statistics (Issue #22) */

#include "latency.h"

uint64_t lat_cyc_to_us(uint64_t cycles, uint64_t freq)
{
    if (freq == 0)
        return 0;
    /* us = cycles * 1e6 / freq, computed without overflowing 64 bits:
     * split into whole seconds and the sub-second remainder. */
    uint64_t whole = cycles / freq;
    uint64_t rem   = cycles % freq;
    return whole * 1000000ull + (rem * 1000000ull + freq / 2) / freq;
}

uint64_t lat_isqrt(uint64_t n)
{
    if (n == 0)
        return 0;
    uint64_t x = n;
    uint64_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

void lat_init(lat_stats_t *s, uint64_t cntfrq)
{
    s->freq     = cntfrq;
    s->prev     = 0;
    s->started  = 0;
    s->n        = 0;
    s->head     = 0;
    s->wake_max = 0;
    s->wake_sum = 0;
    s->wake_n   = 0;
    for (uint32_t i = 0; i < LAT_WINDOW; i++)
        s->ring[i] = 0;
}

uint64_t lat_record(lat_stats_t *s, uint64_t now)
{
    if (!s->started) {
        s->started = 1;
        s->prev = now;
        return 0;                  /* no previous callback to delta against */
    }
    uint64_t delta = now - s->prev;
    s->prev = now;

    s->ring[s->head] = delta;
    s->head = (s->head + 1u) % LAT_WINDOW;
    if (s->n < LAT_WINDOW)
        s->n++;
    return delta;
}

void lat_record_wakeup(lat_stats_t *s, uint64_t cycles)
{
    if (cycles > s->wake_max)
        s->wake_max = cycles;
    s->wake_sum += cycles;
    s->wake_n++;
}

void lat_summary(const lat_stats_t *s, lat_summary_t *out)
{
    out->count       = s->n;
    out->min_us      = 0;
    out->max_us      = 0;
    out->mean_us     = 0;
    out->stddev_us   = 0;
    out->wake_max_us = lat_cyc_to_us(s->wake_max, s->freq);
    out->wake_mean_us = s->wake_n ? lat_cyc_to_us(s->wake_sum / s->wake_n, s->freq)
                                  : 0;
    if (s->n == 0)
        return;

    uint64_t mn = s->ring[0], mx = s->ring[0], sum = 0, sumsq = 0;
    for (uint32_t i = 0; i < s->n; i++) {
        uint64_t d = s->ring[i];
        if (d < mn) mn = d;
        if (d > mx) mx = d;
        sum   += d;
        sumsq += d * d;
    }
    uint64_t mean = sum / s->n;

    /* Variance = E[x^2] - E[x]^2 (clamped against rounding underflow). */
    uint64_t mean_sq = sumsq / s->n;
    uint64_t var = (mean_sq > mean * mean) ? mean_sq - mean * mean : 0;
    uint64_t stddev = lat_isqrt(var);

    out->min_us    = lat_cyc_to_us(mn, s->freq);
    out->max_us    = lat_cyc_to_us(mx, s->freq);
    out->mean_us   = lat_cyc_to_us(mean, s->freq);
    out->stddev_us = lat_cyc_to_us(stddev, s->freq);
}
