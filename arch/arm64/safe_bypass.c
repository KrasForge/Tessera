/* arch/arm64/safe_bypass.c - never-go-silent safe-mode bypass (Theme A) */

#include "safe_bypass.h"

void sb_init(sb_state_t *s)
{
    s->bypassed      = 0;
    s->bypass_blocks = 0;
    s->normal_blocks = 0;
}

int sb_resolve(sb_state_t *s, int alive,
               const uint32_t *node_out, const uint32_t *node_in,
               uint32_t *dst, uint32_t n_words)
{
    /* A node that has ever died stays dead until reloaded (latched). */
    int dead = s->bypassed || !alive;

    if (dead) {
        if (node_in) {
            for (uint32_t i = 0; i < n_words; i++)   /* dry passthrough */
                dst[i] = node_in[i];
        } else {
            for (uint32_t i = 0; i < n_words; i++)   /* no upstream: silence */
                dst[i] = 0u;
        }
        s->bypassed = 1;
        s->bypass_blocks++;
        return 1;
    }

    for (uint32_t i = 0; i < n_words; i++)            /* live effect output */
        dst[i] = node_out[i];
    s->normal_blocks++;
    return 0;
}
