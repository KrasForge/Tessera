/* tests/arm64/sandbox_test.c - host unit tests for the sandbox audit helpers
 * (Issue #35).
 *
 * Exercises the pure classification logic the page-table walk is built on:
 * region containment (sandbox_va_allowed) and leaf-descriptor attribute decode
 * (sandbox_classify_pte, which flags device/MMIO memory and W^X pages).  The
 * full address-space walk is covered on QEMU virt (test-arm-sandbox-qemu).
 *
 * Build/run via:  make test-arm-sandbox
 */

#include "sandbox.h"
#include "mmu.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Build a leaf page descriptor the way vmm_flags_to_attr() does. */
static uint64_t pte_user_rw(void)
{ return PTE_VALID | PTE_AF | PTE_SH_IS | PTE_ATTRIDX(MAIR_IDX_NORMAL) |
         PTE_AP_RW_ALL | PTE_PXN | PTE_UXN | PTE_NG; }
static uint64_t pte_user_rx(void)
{ return PTE_VALID | PTE_AF | PTE_SH_IS | PTE_ATTRIDX(MAIR_IDX_NORMAL) |
         PTE_AP_RO_ALL | PTE_PXN | PTE_NG; }            /* UXN clear -> exec */
static uint64_t pte_user_rwx(void)
{ return PTE_VALID | PTE_AF | PTE_SH_IS | PTE_ATTRIDX(MAIR_IDX_NORMAL) |
         PTE_AP_RW_ALL | PTE_PXN | PTE_NG; }            /* W and X at EL0 */
static uint64_t pte_device(void)
{ return PTE_VALID | PTE_AF | PTE_ATTRIDX(MAIR_IDX_DEVICE) |
         PTE_AP_RW_ALL | PTE_PXN | PTE_UXN | PTE_NG; }

int main(void)
{
    printf("=== Tessera sandbox audit-helper tests (issue #35) ===\n");

    /* ---- region containment ---- */
    sandbox_region_t allow[] = {
        { 0x8000000000ull, 0x2000 },           /* two pages of code */
        { 0x8009000000ull, 0x1000 },           /* trampoline page   */
    };
    int n = 2;
    CHECK(sandbox_va_allowed(0x8000000000ull, allow, n), "first page allowed");
    CHECK(sandbox_va_allowed(0x8000001000ull, allow, n), "second page allowed");
    CHECK(!sandbox_va_allowed(0x8000002000ull, allow, n), "page past region rejected");
    CHECK(sandbox_va_allowed(0x8009000000ull, allow, n), "trampoline page allowed");
    CHECK(!sandbox_va_allowed(0x8040000000ull, allow, n), "unrelated VA rejected");
    CHECK(!sandbox_va_allowed(0x7FFFFFF000ull, allow, n), "VA just below region rejected");

    /* ---- descriptor classification ---- */
    int dev, wx;
    sandbox_classify_pte(pte_user_rw(), &dev, &wx);
    CHECK(!dev && !wx, "RW normal page: not device, not W^X");
    sandbox_classify_pte(pte_user_rx(), &dev, &wx);
    CHECK(!dev && !wx, "RX normal page: not device, not W^X");
    sandbox_classify_pte(pte_user_rwx(), &dev, &wx);
    CHECK(!dev && wx, "RWX page flagged as W^X violation");
    sandbox_classify_pte(pte_device(), &dev, &wx);
    CHECK(dev && !wx, "device page flagged as MMIO");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
