/* tests/arm64/smp_test.c - host unit tests for the audio-core building blocks
 * (Issue #21).
 *
 * The lock-free ring, the watchdog accounting, and the DMA refill are pure C,
 * so the parts that must be correct for a glitch-free audio path are checked
 * on the host - including a real two-thread producer/consumer stress that
 * exercises the cross-core acquire/release ordering of the SPSC ring.
 *
 * Build/run via:  make test-arm-smp
 */

#include "spsc_ring.h"
#include "audio_core.h"

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* ---- single-threaded ring semantics ---- */
static void test_ring_basic(void)
{
    printf("- SPSC ring: FIFO order, wrap, full/empty\n");
    int16_t storage[8];
    spsc_ring_t r;
    spsc_init(&r, storage, 8);            /* usable capacity = 7 */

    CHECK(spsc_available(&r) == 0, "starts empty");
    CHECK(spsc_space(&r) == 7, "space is cap-1");

    int16_t in[7] = {10, 11, 12, 13, 14, 15, 16};
    CHECK(spsc_write(&r, in, 7) == 7, "fills to capacity");
    CHECK(spsc_write(&r, in, 1) == 0, "write on full returns 0");
    CHECK(spsc_available(&r) == 7, "available is full");

    int16_t out[7] = {0};
    CHECK(spsc_read(&r, out, 7) == 7, "drains all");
    int ok = 1;
    for (int i = 0; i < 7; i++) if (out[i] != in[i]) ok = 0;
    CHECK(ok, "data comes out in FIFO order");
    CHECK(spsc_read(&r, out, 1) == 0, "read on empty returns 0");

    /* Force a wrap: head/tail advance past the buffer end. */
    spsc_write(&r, in, 5);
    spsc_read(&r, out, 5);
    spsc_write(&r, in, 6);                /* wraps around the end */
    int16_t w[6] = {0};
    CHECK(spsc_read(&r, w, 6) == 6, "reads across the wrap");
    ok = 1;
    for (int i = 0; i < 6; i++) if (w[i] != in[i]) ok = 0;
    CHECK(ok, "wrapped data is intact and in order");
}

/* ---- two-thread cross-core stress ---- */
#define STRESS_CAP   1024
#define STRESS_TOTAL 4000000

static int16_t      g_stress_buf[STRESS_CAP];
static spsc_ring_t  g_stress;

static void *producer(void *unused)
{
    (void)unused;
    uint32_t n = 0;
    while (n < STRESS_TOTAL) {
        int16_t s = (int16_t)(n & 0x7FFF);
        if (spsc_write(&g_stress, &s, 1) == 1)
            n++;
        /* else ring full: spin until the consumer drains */
    }
    return 0;
}

static void test_ring_threads(void)
{
    printf("- SPSC ring: 2-thread producer/consumer integrity (%d samples)\n",
           STRESS_TOTAL);
    spsc_init(&g_stress, g_stress_buf, STRESS_CAP);

    pthread_t prod;
    pthread_create(&prod, 0, producer, 0);

    uint32_t got = 0;
    int order_ok = 1;
    while (got < STRESS_TOTAL) {
        int16_t s;
        if (spsc_read(&g_stress, &s, 1) == 1) {
            if (s != (int16_t)(got & 0x7FFF))
                order_ok = 0;
            got++;
        }
    }
    pthread_join(prod, 0);

    CHECK(got == STRESS_TOTAL, "consumer received every sample (no loss)");
    CHECK(order_ok, "samples arrived strictly in order (no tearing/reorder)");
}

/* ---- watchdog ---- */
static void test_watchdog(void)
{
    printf("- audio watchdog: overrun accounting\n");
    audio_wd_t wd;
    audio_wd_init(&wd, 1000);             /* 1000-cycle budget */

    CHECK(audio_wd_account(&wd, 800) == 0, "under-budget callback is not an overrun");
    CHECK(audio_wd_account(&wd, 1000) == 0, "exactly-budget callback is not an overrun");
    CHECK(audio_wd_account(&wd, 1200) == 1, "over-budget callback flags an overrun");
    CHECK(wd.overruns == 1, "overrun counter incremented once");
    CHECK(wd.worst == 1200, "worst-case service time tracked");
    CHECK(wd.count == 3, "all callbacks counted");
}

/* ---- refill (underrun -> silence) ---- */
static void test_refill(void)
{
    printf("- audio refill: ring drain + underrun silence\n");
    int16_t storage[64];
    spsc_ring_t r;
    spsc_init(&r, storage, 64);

    int16_t dma[16];
    audio_core_t ac;
    audio_core_init(&ac, &r, dma, 4, 1000);   /* 4 frames -> 8 samples/refill */

    int16_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    spsc_write(&r, src, 8);
    audio_core_refill(&ac, 500);
    int ok = 1;
    for (int i = 0; i < 8; i++) if (dma[i] != src[i]) ok = 0;
    CHECK(ok, "refill copies a full block from the ring");
    CHECK(ac.serviced == 1, "callback counted");

    /* Now the ring is empty: the next refill must zero-fill (silence). */
    for (int i = 0; i < 8; i++) dma[i] = 0x7FFF;
    audio_core_refill(&ac, 500);
    ok = 1;
    for (int i = 0; i < 8; i++) if (dma[i] != 0) ok = 0;
    CHECK(ok, "underrun produces silence, not stale samples");
}

int main(void)
{
    printf("=== Tessera audio-core unit tests (issue #21) ===\n");

    test_ring_basic();
    test_ring_threads();
    test_watchdog();
    test_refill();

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
