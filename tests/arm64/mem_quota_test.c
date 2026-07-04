/* tests/arm64/mem_quota_test.c - host unit tests for the per-plugin memory
 * quota accounting (Theme A: reliability).
 *
 * The charge/limit math is pure, so it is checked here for exact behaviour:
 * charging within budget, refusing an over-budget charge without partial
 * charging, unlimited mode, release, and the bytes->pages ceiling.
 *
 * Build/run via:  make test-arm-mem-quota
 */

#include "mem_quota.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static void test_bytes_to_pages(void)
{
    printf("- bytes -> pages (ceiling)\n");
    CHECK(mq_bytes_to_pages(0) == 0,     "0 bytes = 0 pages");
    CHECK(mq_bytes_to_pages(1) == 1,     "1 byte = 1 page");
    CHECK(mq_bytes_to_pages(4096) == 1,  "4096 bytes = 1 page");
    CHECK(mq_bytes_to_pages(4097) == 2,  "4097 bytes = 2 pages");
    CHECK(mq_bytes_to_pages(256u*1024) == 64, "256 KiB = 64 pages");
}

static void test_within_budget(void)
{
    printf("- charging within budget\n");
    mem_quota_t q; mq_init(&q, 16);
    CHECK(mq_charge(&q, 6) == 1, "charge 6/16 allowed");
    CHECK(q.used_pages == 6,     "used = 6");
    CHECK(mq_charge(&q, 10) == 1, "charge 10 more (16/16) allowed");
    CHECK(q.used_pages == 16,    "used = 16 (exactly at limit)");
    CHECK(q.denied == 0,         "nothing denied");
}

static void test_over_budget_refused(void)
{
    printf("- an over-budget charge is refused, nothing partially charged\n");
    mem_quota_t q; mq_init(&q, 16);
    CHECK(mq_charge(&q, 10) == 1, "charge 10/16 allowed");
    CHECK(mq_charge(&q, 7) == 0,  "charge 7 more (would be 17) refused");
    CHECK(q.used_pages == 10,     "used stays 10 (no partial charge)");
    CHECK(q.denied == 1,          "one denial counted");
    CHECK(mq_charge(&q, 6) == 1,  "a fitting charge still works afterwards");
    CHECK(q.used_pages == 16,     "used = 16");
}

static void test_unlimited(void)
{
    printf("- unlimited (limit 0) never refuses\n");
    mem_quota_t q; mq_init(&q, 0);
    CHECK(mq_charge(&q, 1000000) == 1, "huge charge allowed when unlimited");
    CHECK(q.denied == 0,               "no denials");
}

static void test_release(void)
{
    printf("- release frees budget (clamped at zero)\n");
    mem_quota_t q; mq_init(&q, 16);
    mq_charge(&q, 12);
    mq_release(&q, 4);
    CHECK(q.used_pages == 8, "12 - 4 = 8 used");
    CHECK(mq_charge(&q, 8) == 1, "8 more fits (16/16)");
    mq_release(&q, 100);
    CHECK(q.used_pages == 0, "over-release clamps to 0");
}

int main(void)
{
    printf("=== Tessera per-plugin memory-quota tests (Theme A) ===\n");
    test_bytes_to_pages();
    test_within_budget();
    test_over_budget_refused();
    test_unlimited();
    test_release();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
