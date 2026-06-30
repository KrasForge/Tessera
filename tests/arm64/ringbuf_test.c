/* tests/arm64/ringbuf_test.c - host unit tests for the shared audio ring
 * buffer (Issue #25).
 *
 * Checks FIFO integrity, wrap-around, the overflow/underflow diagnostics, the
 * crash-resilient read path (uninitialised or corrupted indices yield silence),
 * and a real two-thread producer/consumer stress that exercises the cross-core
 * acquire/release ordering.
 *
 * Build/run via:  make test-arm-ringbuf
 */

#include "audio_ringbuf.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define CAP 8u

static unsigned char g_store[sizeof(audio_ring_hdr_t) + CAP * 2u * sizeof(float)];

/* Build a frame whose L=val, R=val+0.5 so order/integrity is checkable. */
static void mkframe(float *f, float val) { f[0] = val; f[1] = val + 0.5f; }

static void test_fifo_and_wrap(void)
{
    printf("- FIFO order, wrap, overflow/underflow counters\n");
    audio_ring_hdr_t *h = (audio_ring_hdr_t *)g_store;
    arb_init(h, CAP);
    CHECK(h->magic == ARB_MAGIC, "init sets the magic");
    CHECK(arb_available(h) == 0 && arb_space(h) == CAP, "starts empty, full space");

    float in[CAP * 2], out[CAP * 2];
    for (uint32_t i = 0; i < CAP; i++) mkframe(&in[i * 2], (float)i);

    CHECK(arb_write(h, in, CAP) == CAP, "fills to capacity");
    CHECK(arb_write(h, in, 1) == 0, "write on full returns 0");
    CHECK(h->overflow == 1, "overflow counts the dropped frame");

    CHECK(arb_read(h, out, CAP) == CAP, "drains all frames");
    int ok = 1;
    for (uint32_t i = 0; i < CAP; i++)
        if (out[i * 2] != (float)i || out[i * 2 + 1] != (float)i + 0.5f) ok = 0;
    CHECK(ok, "frames come out in FIFO order, both channels intact");

    /* Force a wrap and read a partial-underflow block. */
    arb_write(h, in, 5);
    uint32_t got = arb_read(h, out, CAP);    /* ask 8, only 5 available */
    CHECK(got == 5, "read returns the real frame count (5)");
    CHECK(out[5 * 2] == 0.0f && out[7 * 2 + 1] == 0.0f, "shortfall is silence");
    CHECK(h->underflow == 3, "underflow counts the 3 silent frames");
}

static void test_crash_resilience(void)
{
    printf("- crash resilience: stale / uninitialised indices -> silence\n");
    audio_ring_hdr_t *h = (audio_ring_hdr_t *)g_store;
    arb_init(h, CAP);

    float in[CAP * 2], out[CAP * 2];
    for (uint32_t i = 0; i < CAP; i++) mkframe(&in[i * 2], (float)i);
    arb_write(h, in, 4);

    /* Simulate a producer that crashed mid-update leaving an impossible
     * write index far ahead of read. */
    h->write_idx = h->read_idx + CAP + 100u;
    for (uint32_t i = 0; i < CAP * 2; i++) out[i] = 1.0f;
    uint32_t got = arb_read(h, out, CAP);
    CHECK(got == 0, "corrupt indices yield no real frames");
    int silent = 1;
    for (uint32_t i = 0; i < CAP * 2; i++) if (out[i] != 0.0f) silent = 0;
    CHECK(silent, "host gets silence, not garbage");
    CHECK(h->read_idx == h->write_idx, "read index resynchronised to write index");

    /* Uninitialised region (magic cleared) also yields silence. */
    h->magic = 0;
    for (uint32_t i = 0; i < CAP * 2; i++) out[i] = 2.0f;
    CHECK(arb_read(h, out, CAP) == 0, "uninitialised ring reads as 0 frames");
    CHECK(out[0] == 0.0f, "uninitialised ring yields silence");
}

/* ---- two-thread stress ---- */
#define SCAP   1024u
#define STOTAL 2000000u
static unsigned char g_sstore[sizeof(audio_ring_hdr_t) + SCAP * 2u * sizeof(float)];

static void *producer(void *unused)
{
    (void)unused;
    audio_ring_hdr_t *h = (audio_ring_hdr_t *)g_sstore;
    uint32_t n = 0;
    while (n < STOTAL) {
        float f[2]; mkframe(f, (float)(n & 0xffff));
        if (arb_write(h, f, 1) == 1) n++;
    }
    return 0;
}

static void test_threads(void)
{
    printf("- two-thread producer/consumer integrity (%u frames)\n", STOTAL);
    audio_ring_hdr_t *h = (audio_ring_hdr_t *)g_sstore;
    arb_init(h, SCAP);

    pthread_t prod;
    pthread_create(&prod, 0, producer, 0);

    uint32_t got = 0; int order_ok = 1;
    while (got < STOTAL) {
        float f[2];
        if (arb_read(h, f, 1) == 1) {
            float want = (float)(got & 0xffff);
            if (f[0] != want || f[1] != want + 0.5f) order_ok = 0;
            got++;
        }
    }
    pthread_join(prod, 0);
    CHECK(got == STOTAL, "consumer received every frame (no loss)");
    CHECK(order_ok, "frames arrived strictly in order (no tearing)");
    /* The over/underflow counters legitimately tick whenever the spinning
     * producer/consumer momentarily finds the ring full/empty; that is
     * diagnostic, not data loss.  The integrity checks above are the real
     * lock-free correctness proof. */
}

int main(void)
{
    printf("=== Tessera shared-ring tests (issue #25) ===\n");
    test_fifo_and_wrap();
    test_crash_resilience();
    test_threads();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
