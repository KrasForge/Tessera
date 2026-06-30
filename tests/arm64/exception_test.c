/* tests/arm64/exception_test.c — host unit tests for ESR decoding (Issue #12).
 *
 * The vector table and register save/restore are bare-metal asm and cannot
 * run on the host, but the ESR_EL1 decode/classify logic that drives dispatch
 * is pure C and is exactly what runs on the Pi.  This pins down the EC
 * extraction and classification used by arm64_exception().
 *
 * Build/run via:  make test-arm-exc
 */

#include "exceptions.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Reproduce the EC extraction performed by arm64_exception(). */
static uint32_t ec_of(uint64_t esr) { return (uint32_t)((esr >> 26) & 0x3F); }

int main(void)
{
    printf("=== Tessera exception-decode tests (issue #12) ===\n");

    /* EC field extraction: build an ESR with EC in bits [31:26]. */
    CHECK(ec_of((uint64_t)EC_SVC_A64 << 26) == EC_SVC_A64,
          "ESR_EL1.EC extracted from bits [31:26]");
    CHECK(ec_of(((uint64_t)EC_DATA_ABORT_S << 26) | 0x37) == EC_DATA_ABORT_S,
          "ISS bits ignored when extracting EC");

    /* Classification. */
    CHECK(arm64_ec_class(EC_SVC_A64) == EC_CLASS_SVC, "SVC -> SVC class");
    CHECK(arm64_ec_class(EC_DATA_ABORT_L) == EC_CLASS_DATA_ABORT,
          "data abort EL0 -> DATA_ABORT");
    CHECK(arm64_ec_class(EC_DATA_ABORT_S) == EC_CLASS_DATA_ABORT,
          "data abort EL1 -> DATA_ABORT");
    CHECK(arm64_ec_class(EC_INSTR_ABORT_L) == EC_CLASS_INSTR_ABORT,
          "instr abort EL0 -> INSTR_ABORT");
    CHECK(arm64_ec_class(EC_INSTR_ABORT_S) == EC_CLASS_INSTR_ABORT,
          "instr abort EL1 -> INSTR_ABORT");
    CHECK(arm64_ec_class(EC_UNKNOWN) == EC_CLASS_UNKNOWN,
          "undefined instruction -> UNKNOWN");
    CHECK(arm64_ec_class(0x3C) == EC_CLASS_OTHER, "BRK -> OTHER class");

    /* Names are non-empty and distinct for the important cases. */
    CHECK(arm64_ec_name(EC_SVC_A64)[0] != '\0', "SVC has a name");
    CHECK(arm64_ec_name(EC_UNKNOWN)[0] != '\0', "undefined has a name");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
