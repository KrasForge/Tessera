/* arch/arm64/mmu.h — ARMv8-A stage-1 translation tables (Issues #8 / #9)
 *
 * Replaces the x86_64 PML4 page tables (kernel/vmm.c) with the AArch64
 * 4 KiB-granule, 4-level (L0-L3) translation-table format.  Builds an
 * identity-mapped kernel address space and enables the MMU with caches.
 */

#ifndef ARM64_MMU_H
#define ARM64_MMU_H

#include <stdint.h>

/* MAIR_EL1 attribute indices (see mmu.c for the encodings). */
#define MAIR_IDX_NORMAL  0   /* Normal, inner/outer write-back, RW-allocate */
#define MAIR_IDX_DEVICE  1   /* Device-nGnRE (MMIO)                          */

/* ----------------------------------------------------------------------- *
 * ARMv8 stage-1 block/page/table descriptor bit fields.
 * ----------------------------------------------------------------------- */
#define PTE_VALID       (1UL << 0)   /* Descriptor is valid                 */
#define PTE_TABLE       (1UL << 1)   /* L0-L2: table descriptor             */
#define PTE_PAGE        (1UL << 1)   /* L3:    page descriptor (same bit)    */
/* (A block descriptor at L1/L2 leaves bit 1 clear.)                        */

#define PTE_ATTRIDX(i)  ((uint64_t)((i) & 7) << 2)  /* AttrIndx -> MAIR slot */
#define PTE_NS          (1UL << 5)    /* Non-secure                          */
#define PTE_AP_RW_EL1   (0UL << 6)    /* EL1 read/write, EL0 none            */
#define PTE_AP_RW_ALL   (1UL << 6)    /* EL1+EL0 read/write                  */
#define PTE_AP_RO_EL1   (2UL << 6)    /* EL1 read-only, EL0 none             */
#define PTE_AP_RO_ALL   (3UL << 6)    /* EL1+EL0 read-only                   */
#define PTE_SH_IS       (3UL << 8)    /* Inner shareable                     */
#define PTE_AF          (1UL << 10)   /* Access flag (must be 1)             */
#define PTE_NG          (1UL << 11)   /* Not-global (ASID-tagged mapping)    */
#define PTE_PXN         (1UL << 53)   /* Privileged execute-never            */
#define PTE_UXN         (1UL << 54)   /* Unprivileged execute-never          */

/* Output-address field of a descriptor: bits [47:12]. */
#define PTE_ADDR_MASK   0x0000FFFFFFFFF000UL

/* Build the identity-mapped kernel translation tables and enable the MMU,
 * data cache, and instruction cache.  pmm_init() must already have run. */
void mmu_init(void);

/* The kernel's L0 translation-table root (physical == virtual, identity). */
uint64_t *mmu_kernel_pgd(void);

/* Walk to the L3 entry for va, allocating intermediate tables when
 * create is non-zero.  Returns a pointer to the 8-byte L3 entry, or NULL. */
uint64_t *mmu_walk_l3(uint64_t *pgd, uintptr_t va, int create);

/* Map / unmap a single 4 KiB page.  attr is a descriptor attribute mask
 * (PTE_* bits, excluding the output address).  Returns 0 on success. */
int mmu_map_page(uint64_t *pgd, uintptr_t va, uintptr_t pa, uint64_t attr);
int mmu_unmap_page(uint64_t *pgd, uintptr_t va);

/* Resolve va to its physical address in the table rooted at pgd, walking
 * through 1 GiB / 2 MiB block descriptors and 4 KiB page descriptors alike.
 * Returns the physical address (including the in-page offset), or 0 if va is
 * not mapped. */
uintptr_t mmu_translate(uint64_t *pgd, uintptr_t va);

#endif /* ARM64_MMU_H */
