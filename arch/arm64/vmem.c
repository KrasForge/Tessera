/* arch/arm64/vmem.c — VMM API: map/unmap/protect, TLB, ASID (Issue #10)
 *
 * High-level virtual-memory operations layered on the ARMv8 translation
 * tables from mmu.c.  Replaces the x86 invlpg/cr3 logic of kernel/vmm.c
 * with ARM TLBI maintenance and ASID-tagged address spaces.
 */

#include "vmem.h"
#include "mmu.h"
#include "pmm.h"
#include <stdint.h>
#include <stddef.h>

/* --- TLB maintenance ---------------------------------------------------- */

#ifdef HOSTTEST
/* Host unit-test build: no TLB to maintain. */
static inline void tlb_inval_va(uintptr_t va) { (void)va; }
void tlb_flush_all(void) { }
#else
static inline void tlb_inval_va(uintptr_t va)
{
    /* Order the preceding table store, invalidate the stale VA across the
     * inner-shareable domain, then re-synchronise. */
    __asm__ volatile("dsb ishst");
    __asm__ volatile("tlbi vae1is, %0" :: "r"(va >> PAGE_SHIFT));
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}

void tlb_flush_all(void)
{
    __asm__ volatile("dsb ishst");
    __asm__ volatile("tlbi vmalle1is");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}
#endif /* HOSTTEST */

/* --- flag translation --------------------------------------------------- */

uint64_t vmm_flags_to_attr(unsigned flags)
{
    uint64_t a = PTE_VALID | PTE_AF;

    if (flags & VMM_DEVICE)
        a |= PTE_ATTRIDX(MAIR_IDX_DEVICE);
    else
        a |= PTE_ATTRIDX(MAIR_IDX_NORMAL) | PTE_SH_IS;

    int writable = flags & VMM_WRITE;
    if (flags & VMM_USER)
        a |= writable ? PTE_AP_RW_ALL : PTE_AP_RO_ALL;
    else
        a |= writable ? PTE_AP_RW_EL1 : PTE_AP_RO_EL1;

    if (flags & VMM_EXEC) {
        /* Allow execution at the requested privilege; forbid the other. */
        a |= (flags & VMM_USER) ? PTE_PXN : PTE_UXN;
    } else {
        a |= PTE_PXN | PTE_UXN;
    }

    if (flags & VMM_USER)
        a |= PTE_NG;   /* ASID-tagged, not a global mapping */

    return a;
}

/* --- map / unmap into an arbitrary table -------------------------------- */
/*
 * The *_into / *_from variants operate on a caller-supplied L0 root and do
 * NOT perform TLB maintenance — they are used to populate a process address
 * space that is not currently installed in TTBR0_EL1 (see arch/arm64/
 * process.c).  The kernel-facing vmm_map / vmm_unmap wrappers below target
 * the active kernel table and invalidate the TLB per page.
 */
int vmm_map_into(uint64_t *pgd, uintptr_t pa, uintptr_t va, size_t size,
                 unsigned flags)
{
    if ((pa | va | size) & (PAGE_SIZE - 1))
        return -1;

    uint64_t attr = vmm_flags_to_attr(flags);

    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        if (mmu_map_page(pgd, va + off, pa + off, attr) != 0) {
            for (size_t done = 0; done < off; done += PAGE_SIZE)
                mmu_unmap_page(pgd, va + done);
            return -1;
        }
    }
    return 0;
}

int vmm_unmap_from(uint64_t *pgd, uintptr_t va, size_t size)
{
    if ((va | size) & (PAGE_SIZE - 1))
        return -1;

    int rc = 0;
    for (size_t off = 0; off < size; off += PAGE_SIZE)
        if (mmu_unmap_page(pgd, va + off) != 0)
            rc = -1;
    return rc;
}

/* --- map / unmap / protect (active kernel table) ------------------------ */

int vmm_map(uintptr_t pa, uintptr_t va, size_t size, unsigned flags)
{
    uint64_t *pgd = mmu_kernel_pgd();
    if (vmm_map_into(pgd, pa, va, size, flags) != 0)
        return -1;
    for (size_t off = 0; off < size; off += PAGE_SIZE)
        tlb_inval_va(va + off);
    return 0;
}

int vmm_unmap(uintptr_t va, size_t size)
{
    uint64_t *pgd = mmu_kernel_pgd();
    int rc = vmm_unmap_from(pgd, va, size);
    for (size_t off = 0; off < size; off += PAGE_SIZE)
        tlb_inval_va(va + off);
    return rc;
}

int vmm_protect(uintptr_t va, size_t size, unsigned flags)
{
    if ((va | size) & (PAGE_SIZE - 1))
        return -1;

    uint64_t *pgd = mmu_kernel_pgd();
    uint64_t attr = vmm_flags_to_attr(flags);

    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t *e = mmu_walk_l3(pgd, va + off, 0);
        if (!e || !(*e & PTE_VALID))
            return -1;
        uintptr_t pa = (uintptr_t)(*e & PTE_ADDR_MASK);
        *e = (pa & PTE_ADDR_MASK) | attr | PTE_PAGE | PTE_VALID;
        tlb_inval_va(va + off);
    }
    return 0;
}

/* --- ASID allocation ---------------------------------------------------- */

#define ASID_COUNT 256
static uint8_t g_asid_used[ASID_COUNT / 8];   /* bit set = ASID in use */

uint16_t asid_alloc(void)
{
    /* ASID 0 is reserved for the global kernel mapping. */
    for (uint16_t a = 1; a < ASID_COUNT; a++) {
        if (!(g_asid_used[a >> 3] & (1u << (a & 7)))) {
            g_asid_used[a >> 3] |= (1u << (a & 7));
            return a;
        }
    }
    return 0;
}

void asid_free(uint16_t asid)
{
    if (asid == 0 || asid >= ASID_COUNT)
        return;
    g_asid_used[asid >> 3] &= ~(1u << (asid & 7));
}

uint64_t vmm_make_ttbr(uintptr_t table_pa, uint16_t asid)
{
    return ((uint64_t)table_pa & PTE_ADDR_MASK) | ((uint64_t)asid << 48);
}
