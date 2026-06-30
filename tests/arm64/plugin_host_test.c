/* tests/arm64/plugin_host_test.c - host unit tests for the resilient audio
 * host (Issue #26).
 *
 * Checks the host's per-block policy: it drains real audio when the plugin is
 * producing, substitutes silence and counts an overrun when the ring is empty,
 * detects producer death (the DEAD status word) and logs it exactly once, and
 * keeps returning silence forever after a crash without stalling.
 *
 * Build/run via:  make test-arm-plugin-host
 */

#include "plugin_host.h"
#include "audio_ringbuf.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Count host_on_death() calls (overrides the weak default). */
static int g_death_calls;
void host_on_death(plugin_host_t *h) { (void)h; g_death_calls++; }

#define CAP 16u
#define BLK 4u
static unsigned char g_store[sizeof(audio_ring_hdr_t) + CAP * 2u * sizeof(float)];

static int block_is_silent(const float *b, uint32_t frames)
{
    for (uint32_t i = 0; i < frames * 2u; i++)
        if (b[i] != 0.0f) return 0;
    return 1;
}

int main(void)
{
    printf("=== Tessera plugin-host tests (issue #26) ===\n");

    audio_ring_hdr_t *ring = (audio_ring_hdr_t *)g_store;
    arb_init(ring, CAP);
    CHECK(ring->producer_state == ARB_PRODUCER_ALIVE, "ring starts with a live producer");

    plugin_host_t host;
    host_init(&host, ring, BLK);

    /* Producer writes one block of real audio. */
    float in[BLK * 2], out[BLK * 2];
    for (uint32_t i = 0; i < BLK * 2; i++) in[i] = (float)(i + 1);
    arb_write(ring, in, BLK);

    CHECK(host_block(&host, out) == 1, "full block drained from a live producer");
    CHECK(!block_is_silent(out, BLK), "host output is real audio (non-silent)");
    CHECK(host.overruns == 0, "no overrun when audio is available");

    /* Ring now empty but producer still alive: transient underrun -> silence,
     * counted, but NOT logged as a death. */
    CHECK(host_block(&host, out) == 0, "empty ring yields a silence block");
    CHECK(block_is_silent(out, BLK), "substituted block is silence");
    CHECK(host.overruns == 1, "overrun counted");
    CHECK(g_death_calls == 0 && host.dead_logged == 0, "live-but-empty is not a death");

    /* Kernel marks the producer dead (as on a fault kill). */
    ring->producer_state = ARB_PRODUCER_DEAD;
    CHECK(host_producer_dead(&host), "host sees the DEAD status word");

    CHECK(host_block(&host, out) == 0, "post-death block is silence");
    CHECK(block_is_silent(out, BLK), "post-death output is silence");
    CHECK(g_death_calls == 1, "death logged exactly once");

    /* Many more blocks after death: still silence, never logged again, never
     * stalls or returns real audio. */
    int still_silent = 1;
    for (int i = 0; i < 1000; i++) {
        if (host_block(&host, out) != 0) still_silent = 0;
        if (!block_is_silent(out, BLK)) still_silent = 0;
    }
    CHECK(still_silent, "host keeps emitting silence after a crash (no stall)");
    CHECK(g_death_calls == 1, "death is logged only once, not per block");
    CHECK(host.overruns == 1002, "every silent block is counted as an overrun");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
