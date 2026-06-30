/* arch/arm64/mmu.c — ARMv8-A stage-1 translation tables + MMU enable
 *                     (Issues #8 / #9)
 *
 * Builds an identity-mapped kernel address space using the 4 KiB granule
 * and 4-level (L0-L3) translation-table walk, then turns on the MMU, data
 * cache, and instruction cache.
 *
 * Memory types (MAIR_EL1):
 *   - index 0: Normal, inner+outer write-back, RW-allocate  (RAM)
 *   - index 1: Device-nGnRE                                  (MMIO)
 *
 * The identity map of RAM and MMIO is laid down with 2 MiB L2 block
 * descriptors (compact: a 1 GiB RAM map needs only ~512 entries).  Finer
 * 4 KiB page mappings for the VMM API (issue #10) are created on demand by
 * mmu_map_page(), which walks down to L3.
 */

#include "mmu.h"
#include "pmm.h"
#include "pmem.h"
#include <stdint.h>

#define L2_BLOCK_SIZE   0x200000UL   /* 2 MiB */
#define IDX(va, shift)  (((va) >> (shift)) & 0x1FFUL)

static uint64_t *g_kernel_pgd;       /* L0 root (identity: PA == VA) */

/* Follow the table descriptor at tbl[idx], allocating a fresh zeroed table
 * when it is absent and create != 0.  Returns the next-level table pointer
 * (identity-mapped physical address), or NULL. */
static uint64_t *next_table(uint64_t *tbl, unsigned idx, int create)
{
    uint64_t e = tbl[idx];
    if (e & PTE_VALID)
        return (uint64_t *)P2V((uintptr_t)(e & PTE_ADDR_MASK));

    if (!create)
        return (uint64_t *)0;

    uintptr_t pa = phys_alloc_page_zero();
    if (!pa)
        return (uint64_t *)0;

    tbl[idx] = (pa & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE;
    return (uint64_t *)P2V(pa);
}

uint64_t *mmu_walk_l3(uint64_t *pgd, uintptr_t va, int create)
{
    uint64_t *l1 = next_table(pgd, IDX(va, 39), create);
    if (!l1) return (uint64_t *)0;
    uint64_t *l2 = next_table(l1, IDX(va, 30), create);
    if (!l2) return (uint64_t *)0;
    uint64_t *l3 = next_table(l2, IDX(va, 21), create);
    if (!l3) return (uint64_t *)0;
    return &l3[IDX(va, 12)];
}

int mmu_map_page(uint64_t *pgd, uintptr_t va, uintptr_t pa, uint64_t attr)
{
    uint64_t *e = mmu_walk_l3(pgd, va, 1);
    if (!e)
        return -1;
    *e = (pa & PTE_ADDR_MASK) | attr | PTE_PAGE | PTE_VALID;
    return 0;
}

int mmu_unmap_page(uint64_t *pgd, uintptr_t va)
{
    uint64_t *e = mmu_walk_l3(pgd, va, 0);
    if (!e || !(*e & PTE_VALID))
        return -1;
    *e = 0;
    return 0;
}

/* Install a 2 MiB block descriptor (identity) at L2. */
static int map_block_2m(uintptr_t va, uintptr_t pa, uint64_t attr)
{
    uint64_t *l1 = next_table(g_kernel_pgd, IDX(va, 39), 1);
    if (!l1) return -1;
    uint64_t *l2 = next_table(l1, IDX(va, 30), 1);
    if (!l2) return -1;
    /* Block descriptor: bit 1 (table/page) stays clear. */
    l2[IDX(va, 21)] = (pa & PTE_ADDR_MASK) | attr;
    return 0;
}

#ifdef HOSTTEST
/* Host unit-test build: no real MMU.  The translation tables are still
 * built (and inspected by the test harness); enabling is a no-op. */
static void mmu_enable(void) { }
#else
static void mmu_enable(void)
{
    /* MAIR_EL1: attr0 = Normal WB RW-alloc (0xFF), attr1 = Device-nGnRE (0x04). */
    uint64_t mair = (0xFFUL << (8 * MAIR_IDX_NORMAL)) |
                    (0x04UL << (8 * MAIR_IDX_DEVICE));
    __asm__ volatile("msr mair_el1, %0" :: "r"(mair));

    /* TCR_EL1: 4 KiB granule, 48-bit VA (T0SZ/T1SZ=16), WB-cacheable +
     * inner-shareable table walks, 40-bit IPA (IPS=2). */
    uint64_t tcr =
        (16UL << 0)  |   /* T0SZ  = 16 -> 48-bit VA for TTBR0   */
        (1UL  << 8)  |   /* IRGN0 = WB RW-allocate              */
        (1UL  << 10) |   /* ORGN0 = WB RW-allocate              */
        (3UL  << 12) |   /* SH0   = inner shareable             */
        (0UL  << 14) |   /* TG0   = 4 KiB                       */
        (16UL << 16) |   /* T1SZ  = 16                          */
        (1UL  << 24) |   /* IRGN1 = WB RW-allocate              */
        (1UL  << 26) |   /* ORGN1 = WB RW-allocate              */
        (3UL  << 28) |   /* SH1   = inner shareable             */
        (2UL  << 30) |   /* TG1   = 4 KiB                       */
        (2UL  << 32);    /* IPS   = 40-bit physical address     */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));

    /* TTBR0_EL1 = kernel table root, ASID 0 (global kernel mapping). */
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)(uintptr_t)g_kernel_pgd));

    /* Ensure table writes are visible, then invalidate TLB + I-cache. */
    __asm__ volatile("dsb ish; isb");
    __asm__ volatile("tlbi vmalle1; dsb ish; isb");
    __asm__ volatile("ic iallu; dsb nsh; isb");

    /* Enable MMU (M), data cache (C), instruction cache (I). */
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);
    __asm__ volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr));
}
#endif /* HOSTTEST */

void mmu_init(void)
{
    g_kernel_pgd = (uint64_t *)P2V(phys_alloc_page_zero());

    /* RAM: Normal cacheable, kernel RW, EL1-executable (UXN set). */
    uint64_t normal = PTE_AF | PTE_SH_IS | PTE_AP_RW_EL1 |
                      PTE_ATTRIDX(MAIR_IDX_NORMAL) | PTE_UXN | PTE_VALID;

    /* MMIO: Device-nGnRE, kernel RW, execute-never. */
    uint64_t device = PTE_AF | PTE_AP_RW_EL1 |
                      PTE_ATTRIDX(MAIR_IDX_DEVICE) | PTE_UXN | PTE_PXN | PTE_VALID;

    for (uintptr_t pa = PHYS_RAM_START; pa < PHYS_RAM_END; pa += L2_BLOCK_SIZE)
        map_block_2m(pa, pa, normal);

    for (uintptr_t pa = MMIO_BASE; pa < MMIO_END; pa += L2_BLOCK_SIZE)
        map_block_2m(pa, pa, device);

    mmu_enable();
}

uint64_t *mmu_kernel_pgd(void)
{
    return g_kernel_pgd;
}
