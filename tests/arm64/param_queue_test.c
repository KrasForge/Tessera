/* tests/arm64/param_queue_test.c - host unit tests for the lock-free parameter
 * queue (Issue #30).
 *
 * Checks FIFO delivery, full/empty behaviour, and a two-thread producer/
 * consumer stress that exercises the acquire/release ordering used to deliver
 * a parameter to the audio thread without a lock.
 *
 * Build/run via:  make test-arm-param-queue
 */

#include "param_queue.h"

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define CAP 8u
static unsigned char g_store[sizeof(param_queue_t) + CAP * sizeof(param_event_t)];

static void test_basic(void)
{
    printf("- FIFO delivery, full/empty\n");
    param_queue_t *q = (param_queue_t *)g_store;
    pq_init(q, CAP);
    CHECK(pq_count(q) == 0, "starts empty");

    uint32_t id, bits;
    CHECK(pq_pop(q, &id, &bits) == 0, "pop on empty returns 0");

    for (uint32_t i = 0; i < CAP; i++)
        CHECK(pq_push(q, i, i * 100u) == 1 || i >= CAP, "fill");
    CHECK(pq_push(q, 99u, 99u) == 0, "push on full returns 0");
    CHECK(pq_count(q) == CAP, "count is full");

    int ok = 1;
    for (uint32_t i = 0; i < CAP; i++) {
        if (!pq_pop(q, &id, &bits) || id != i || bits != i * 100u) ok = 0;
    }
    CHECK(ok, "events come out in FIFO order with intact (id,bits)");
    CHECK(pq_pop(q, &id, &bits) == 0, "empty again after draining");
}

#define SCAP   256u
#define STOTAL 1000000u
static unsigned char g_sstore[sizeof(param_queue_t) + SCAP * sizeof(param_event_t)];

static void *producer(void *u)
{
    (void)u;
    param_queue_t *q = (param_queue_t *)g_sstore;
    uint32_t n = 0;
    while (n < STOTAL)
        if (pq_push(q, n, n ^ 0xABCDu)) n++;
    return 0;
}

static void test_threads(void)
{
    printf("- two-thread producer/consumer (%u events)\n", STOTAL);
    param_queue_t *q = (param_queue_t *)g_sstore;
    pq_init(q, SCAP);
    pthread_t p;
    pthread_create(&p, 0, producer, 0);

    uint32_t got = 0, id, bits; int ok = 1;
    while (got < STOTAL)
        if (pq_pop(q, &id, &bits)) {
            if (id != got || bits != (got ^ 0xABCDu)) ok = 0;
            got++;
        }
    pthread_join(p, 0);
    CHECK(got == STOTAL, "every event received");
    CHECK(ok, "events delivered in order, values intact");
}

int main(void)
{
    printf("=== Tessera param-queue tests (issue #30) ===\n");
    test_basic();
    test_threads();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
