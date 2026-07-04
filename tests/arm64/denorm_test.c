/* tests/arm64/denorm_test.c - host unit tests for denormal protection
 * (Theme H, issue #130).
 *
 * Build/run via:  make test-arm-denorm
 */

#include "denorm.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Reinterpret a float as its bit pattern (host only, for building test values). */
static uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void test_fpcr(void)
{
    printf("- FPCR flush-to-zero bit handling\n");
    CHECK(FPCR_FZ == (1u << 24), "FZ is bit 24");
    CHECK(denorm_fpcr_ftz_enabled(0) == 0, "FTZ off in a zero FPCR");
    CHECK(denorm_fpcr_ftz_enabled(denorm_fpcr_set_ftz(0)) == 1, "set_ftz turns it on");
    CHECK(denorm_fpcr_default() == FPCR_FZ, "the kernel default has FTZ set");
    /* Setting FTZ preserves the other rounding/exception bits. */
    uint32_t rmode = 0x00c00000u;   /* RMode = round toward zero */
    CHECK(denorm_fpcr_set_ftz(rmode) == (rmode | FPCR_FZ), "set_ftz leaves other bits intact");
    CHECK(denorm_fpcr_set_ftz(FPCR_FZ) == FPCR_FZ, "set_ftz is idempotent");
}

static void test_subnormal_detect(void)
{
    printf("- subnormal detection on bit patterns\n");
    CHECK(denorm_is_subnormal(0x00000001u) == 1, "smallest positive subnormal");
    CHECK(denorm_is_subnormal(0x007fffffu) == 1, "largest positive subnormal");
    CHECK(denorm_is_subnormal(0x80000001u) == 1, "a negative subnormal");
    CHECK(denorm_is_subnormal(0x00000000u) == 0, "+0 is not subnormal");
    CHECK(denorm_is_subnormal(0x80000000u) == 0, "-0 is not subnormal");
    CHECK(denorm_is_subnormal(0x00800000u) == 0, "smallest normal (FLT_MIN) is not subnormal");
    CHECK(denorm_is_subnormal(bits_of(1.0f)) == 0, "1.0 is not subnormal");
    CHECK(denorm_is_subnormal(0x7f800000u) == 0, "+inf is not subnormal");
}

static void test_flush(void)
{
    printf("- flushing subnormals to signed zero\n");
    CHECK(denorm_flush(0x00000001u) == 0x00000000u, "tiny +subnormal -> +0");
    CHECK(denorm_flush(0x80000001u) == 0x80000000u, "tiny -subnormal -> -0 (sign kept)");
    CHECK(denorm_flush(0x007fffffu) == 0x00000000u, "largest subnormal -> +0");
    /* Normals and specials pass through untouched. */
    CHECK(denorm_flush(bits_of(1.0f)) == bits_of(1.0f), "1.0 passes through");
    CHECK(denorm_flush(0x00800000u) == 0x00800000u, "FLT_MIN passes through");
    CHECK(denorm_flush(0x7f800000u) == 0x7f800000u, "+inf passes through");
    CHECK(denorm_flush(0x80000000u) == 0x80000000u, "-0 passes through unchanged");
}

int main(void)
{
    printf("=== Tessera denormal-protection tests (Theme H, #130) ===\n");
    test_fpcr();
    test_subnormal_detect();
    test_flush();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
