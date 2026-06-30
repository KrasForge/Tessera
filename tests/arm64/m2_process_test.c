/* tests/arm64/m2_process_test.c — host unit tests for process isolation
 *                                  (Issue #11, M2).
 *
 * Compiles the real arch/arm64 process / page-table sources with -DHOSTTEST
 * (see pmem.h) and verifies the issue #11 acceptance criteria:
 *   - back-to-back processes get distinct L0 roots (and ASIDs);
 *   - the kernel (incl. UART MMIO) is reachable in every process;
 *   - user space starts empty and is isolated between processes;
 *   - destroying a process reclaims every page-table page and user frame
 *     with no physical-frame leak.
 *
 * Build/run via:  make test-arm-m2
 */

#define _GNU_SOURCE

#include "process.h"
#include "pmm.h"
#include "mmu.h"
#include "vmem.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>

unsigned char *g_hosttest_ram;          /* simulated physical RAM (pmem.h) */

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define UART_PA   0xFE201000UL           /* PL011 base (identity-mapped)   */
#define KCODE_PA  0x80000UL              /* kernel load address            */

int main(void)
{
    g_hosttest_ram = mmap(NULL, PHYS_RAM_END, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_hosttest_ram == MAP_FAILED) { perror("mmap"); return 2; }

    printf("=== Tessera M2 process-isolation tests (issue #11) ===\n");
    pmm_init();
    mmu_init();

    size_t free_baseline = pmm_free_pages();

    /* --- two address spaces --------------------------------------------- */
    process_t *a = process_create("plugin-a");
    process_t *b = process_create("plugin-b");
    CHECK(a && b, "two processes created");
    CHECK(a->pgd_pa != b->pgd_pa, "distinct L0 roots (different PAs)");
    CHECK(a->asid != b->asid && a->asid && b->asid, "distinct non-zero ASIDs");
    CHECK(a->pid != b->pid, "distinct PIDs");
    CHECK((a->ttbr0 >> 48) == a->asid &&
          (a->ttbr0 & PTE_ADDR_MASK) == (a->pgd_pa & PTE_ADDR_MASK),
          "TTBR0 packs ASID + root PA");

    /* --- kernel reachable in both --------------------------------------- */
    CHECK(mmu_translate(a->pgd, UART_PA) == UART_PA &&
          mmu_translate(b->pgd, UART_PA) == UART_PA,
          "UART MMIO reachable in both processes");
    CHECK(mmu_translate(a->pgd, KCODE_PA) == KCODE_PA &&
          mmu_translate(b->pgd, KCODE_PA) == KCODE_PA,
          "kernel code identity-mapped in both processes");

    /* --- user space starts empty ---------------------------------------- */
    CHECK(mmu_translate(a->pgd, USER_VA_BASE) == 0,
          "user space empty at creation");

    /* --- per-process user mappings are isolated ------------------------- */
    uintptr_t fa = phys_alloc_page();
    uintptr_t fb = phys_alloc_page();
    CHECK(process_map(a, fa, USER_VA_BASE, PAGE_SIZE,
                      VMM_READ | VMM_WRITE) == 0, "map page into A");
    CHECK(process_map(b, fb, USER_VA_BASE, PAGE_SIZE,
                      VMM_READ | VMM_WRITE) == 0, "map page into B (same VA)");
    CHECK(mmu_translate(a->pgd, USER_VA_BASE) == fa, "A sees its own frame");
    CHECK(mmu_translate(b->pgd, USER_VA_BASE) == fb, "B sees its own frame");
    CHECK(fa != fb && mmu_translate(a->pgd, USER_VA_BASE) !=
                      mmu_translate(b->pgd, USER_VA_BASE),
          "same user VA isolated between A and B");

    /* a user mapping in A must not leak into the shared kernel table */
    CHECK(mmu_translate(mmu_kernel_pgd(), USER_VA_BASE) == 0,
          "user mapping absent from the kernel table");

    /* --- reject out-of-range user maps ---------------------------------- */
    CHECK(process_map(a, fa, 0x1000, PAGE_SIZE, VMM_READ) != 0,
          "map below USER_VA_BASE rejected");

    CHECK(process_count() == 2, "process_count reports two live processes");

    /* --- teardown reclaims everything ----------------------------------- */
    process_destroy(a);
    process_destroy(b);
    CHECK(process_count() == 0, "no live processes after destroy");
    CHECK(pmm_free_pages() == free_baseline,
          "all page-table pages and user frames reclaimed (no leak)");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
