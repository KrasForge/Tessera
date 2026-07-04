/* arch/arm64/hot_reload.c - plugin hot-reload without a dropout (Theme A) */

#include "hot_reload.h"

void hr_init(hr_state_t *s)
{
    s->active   = 0u;
    s->pending  = HR_NONE;
    s->phase    = HR_STEADY;
    s->swaps    = 0u;
    s->prepares = 0u;
    s->aborts   = 0u;
}

int hr_prepare(hr_state_t *s)
{
    if (s->phase != HR_STEADY)
        return 0;                 /* a reload is already in flight */
    s->pending = s->active + 1u;
    s->phase   = HR_PREPARING;
    s->prepares++;
    return 1;
}

int hr_ready(hr_state_t *s, int ok)
{
    if (s->phase != HR_PREPARING)
        return (int)s->phase;     /* nothing was preparing */
    if (ok) {
        s->phase = HR_ARMED;      /* new version loaded: swap on the next block */
    } else {
        s->phase   = HR_STEADY;   /* load failed: keep running the old version */
        s->pending = HR_NONE;
        s->aborts++;
    }
    return (int)s->phase;
}

uint32_t hr_next(hr_state_t *s, uint32_t *retired)
{
    if (s->phase == HR_ARMED) {
        *retired  = s->active;    /* the block boundary: commit the swap */
        s->active = s->pending;
        s->pending = HR_NONE;
        s->phase  = HR_STEADY;
        s->swaps++;
    } else {
        *retired = HR_NONE;
    }
    return s->active;
}

int hr_phase(const hr_state_t *s)
{
    return (int)s->phase;
}
