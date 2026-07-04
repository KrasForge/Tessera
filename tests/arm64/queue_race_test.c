/* tests/arm64/queue_race_test.c - concurrency verification of the lock-free
 * queues under ThreadSanitizer (Theme M22, issue #197).
 *
 * The SPSC rings are the load-bearing primitive of the whole wait-free design:
 * the audio thread drains them with no lock and no syscall.  The existing
 * threaded stress (test-arm-smp) runs under ASan, which cannot see a data race
 * or a broken memory ordering; this harness runs a real producer/consumer pair
 * for each ring under -fsanitize=thread, so a missing acquire/release or a torn
 * publish is caught, not merely "didn't happen to manifest".
 *
 * It verifies both the ordering (TSan) and the semantics: every item is received
 * exactly once, in order, untorn.  The 32-bit head/tail counters are pre-seeded
 * near UINT32_MAX so the unsigned index wrap is exercised mid-run.
 *
 * Build/run via:  make test-arm-queue-race   (separate TSan build)
 */

#include "spsc_ring.h"
#include "param_queue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static long g_items = 5000000;   /* items per queue (overridable via argv) */

/* A deterministic value for sequence number `s`, so the consumer can detect a
 * torn or stale payload (a value that does not match its own id). */
static uint32_t mix(uint32_t s) { return s * 2654435761u + 0x9e3779b9u; }

/* ---- SPSC int16 ring ----------------------------------------------------- */

#define RING_CAP 1024

typedef struct { spsc_ring_t *r; long n; } ring_arg_t;

static void *ring_producer(void *v)
{
    ring_arg_t *a = v;
    long seq = 0;
    int16_t buf[64];
    while (seq < a->n) {
        uint32_t chunk = 0;
        while (chunk < 64 && seq + chunk < a->n) {
            buf[chunk] = (int16_t)((uint32_t)(seq + chunk) & 0xffffu);   /* known pattern */
            chunk++;
        }
        uint32_t off = 0;
        while (off < chunk) {
            uint32_t w = spsc_write(a->r, buf + off, chunk - off);
            off += w;                                     /* spin while full */
        }
        seq += chunk;
    }
    return 0;
}

static void test_spsc_ring(void)
{
    printf("- SPSC int16 ring: %ld-sample producer/consumer under TSan\n", g_items);
    static int16_t storage[RING_CAP];
    spsc_ring_t r;
    spsc_init(&r, storage, RING_CAP);
    /* Force the unsigned index wrap partway through the run. */
    r.head = r.tail = 0xffffff00u;

    ring_arg_t arg = { &r, g_items };
    pthread_t prod;
    pthread_create(&prod, 0, ring_producer, &arg);

    long got = 0; int ok = 1;
    int16_t buf[64];
    while (got < g_items) {
        uint32_t rd = spsc_read(&r, buf, 64);
        for (uint32_t i = 0; i < rd; i++) {
            int16_t expect = (int16_t)((uint32_t)(got + i) & 0xffffu);
            if (buf[i] != expect) ok = 0;
        }
        got += rd;                                        /* spin while empty */
    }
    pthread_join(prod, 0);
    CHECK(ok, "every sample received exactly once, in order (no loss/dup/tear)");
    CHECK(got == g_items, "consumed the full stream");
}

/* ---- parameter queue (id, bits) ------------------------------------------ */

typedef struct { param_queue_t *q; long n; } pq_arg_t;

static void *pq_producer(void *v)
{
    pq_arg_t *a = v;
    long seq = 0;
    while (seq < a->n) {
        /* push id=seq, bits=mix(seq); retry (spin) while full. */
        if (pq_push(a->q, (uint32_t)seq, mix((uint32_t)seq)))
            seq++;
    }
    return 0;
}

static void test_param_queue(void)
{
    printf("- parameter queue: %ld-event producer/consumer under TSan\n", g_items);
    #define PQ_CAP 1024
    param_queue_t *q = malloc(pq_bytes(PQ_CAP));
    pq_init(q, PQ_CAP);
    q->head = q->tail = 0xffffff00u;   /* exercise the counter wrap */

    pq_arg_t arg = { q, g_items };
    pthread_t prod;
    pthread_create(&prod, 0, pq_producer, &arg);

    long got = 0; int ok = 1;
    while (got < g_items) {
        uint32_t id, bits;
        if (pq_pop(q, &id, &bits)) {
            /* id must be the next expected, and its payload must be untorn. */
            if (id != (uint32_t)got || bits != mix(id)) ok = 0;
            got++;
        }
    }
    pthread_join(prod, 0);
    free(q);
    CHECK(ok, "every event received once, in order, with an untorn payload");
    CHECK(got == g_items, "consumed the full stream");
}

int main(int argc, char **argv)
{
    if (argc > 1) g_items = atol(argv[1]);
    printf("=== Tessera lock-free queue race verification (M22, #197) ===\n");
    test_spsc_ring();
    test_param_queue();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
