/* arch/arm64/plugin_host.c - resilient audio host (Issue #26, M5) */

#include "plugin_host.h"
#include "audio_ringbuf.h"

/* Weak default: do nothing.  Overridden by the harness/system to log the
 * crash over UART and apply a restart policy. */
__attribute__((weak)) void host_on_death(plugin_host_t *h) { (void)h; }

void host_init(plugin_host_t *h, audio_ring_hdr_t *ring, uint32_t frames)
{
    h->ring        = ring;
    h->frames      = frames;
    h->overruns    = 0;
    h->dead_logged = 0;
}

int host_producer_dead(const plugin_host_t *h)
{
    return __atomic_load_n(&h->ring->producer_state, __ATOMIC_ACQUIRE)
           == ARB_PRODUCER_DEAD;
}

int host_block(plugin_host_t *h, float *out)
{
    /* arb_read fills the block from the ring and silences any shortfall, even
     * for a crashed/corrupt producer (it never reads garbage). */
    uint32_t got = arb_read(h->ring, out, h->frames);

    if (got < h->frames) {
        h->overruns++;
        /* If the kernel has marked the producer dead, log it once and keep
         * going on silence; the host must not stall or crash. */
        if (host_producer_dead(h) && !h->dead_logged) {
            h->dead_logged = 1;
            host_on_death(h);
        }
        return 0;
    }
    return 1;
}
