/* arch/arm64/process.c — per-process address space + PCB (Issue #11, M2)
 *
 * Replaces the x86 cr3-based process model of IKOS kernel/process.c with an
 * AArch64 TTBR0_EL1 design: each process owns an L0 translation-table root,
 * shares the kernel's mappings, and carries a unique ASID.
 */

#include "process.h"
#include "pmm.h"
#include "pmem.h"
#include "mmu.h"
#include "vmem.h"
#include <stdint.h>
#include <stddef.h>

#define MAX_PROCESSES   64
#define PTE_PER_TABLE   512
/* L0 index of the first user entry — everything below it is shared kernel. */
#define USER_L0_FIRST   (USER_VA_BASE >> 39)   /* == 1 */

static process_t g_proc[MAX_PROCESSES];
static uint32_t  g_next_pid = 1;

static void copy_name(char *dst, const char *src)
{
    int i = 0;
    if (src)
        for (; src[i] && i < 15; i++)
            dst[i] = src[i];
    dst[i] = '\0';
}

process_t *process_create(const char *name)
{
    process_t *p = (process_t *)0;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (g_proc[i].state == PROC_UNUSED) { p = &g_proc[i]; break; }
    if (!p)
        return (process_t *)0;

    uint16_t asid = asid_alloc();
    if (asid == 0)
        return (process_t *)0;

    uintptr_t pgd_pa = phys_alloc_page_zero();
    if (!pgd_pa) {
        asid_free(asid);
        return (process_t *)0;
    }
    uint64_t *pgd = (uint64_t *)P2V(pgd_pa);

    /* Share the kernel's top-level entries so kernel code/data and MMIO
     * (e.g. the UART) stay reachable once this process is installed; user
     * space (L0[USER_L0_FIRST..]) is left empty. */
    uint64_t *kpgd = mmu_kernel_pgd();
    for (unsigned i = 0; i < USER_L0_FIRST; i++)
        pgd[i] = kpgd[i];

    p->pid    = g_next_pid++;
    p->asid   = asid;
    p->pgd_pa = pgd_pa;
    p->pgd    = pgd;
    p->ttbr0  = vmm_make_ttbr(pgd_pa, asid);
    p->state  = PROC_READY;
    copy_name(p->name, name);
    return p;
}

int process_map(process_t *p, uintptr_t pa, uintptr_t va, size_t size,
                unsigned flags)
{
    if (!p || p->state == PROC_UNUSED)
        return -1;
    if (va < USER_VA_BASE || va >= USER_VA_END || (va + size) > USER_VA_END)
        return -1;
    return vmm_map_into(p->pgd, pa, va, size, flags | VMM_USER);
}

/* Recursively reclaim everything beneath a translation table.  `level` is
 * the level of `table` itself: 1 = L1, 2 = L2, 3 = L3.  At L3 the leaf
 * descriptors map user data frames, which are freed; at L1/L2 the child
 * table pages are freed after their subtrees. */
static void free_subtree(uint64_t *table, int level)
{
    for (int i = 0; i < PTE_PER_TABLE; i++) {
        uint64_t e = table[i];
        if (!(e & PTE_VALID))
            continue;
        uintptr_t pa = (uintptr_t)(e & PTE_ADDR_MASK);
        if (level == 3) {
            phys_free_page(pa);                 /* user data frame */
        } else if (e & PTE_TABLE) {
            free_subtree((uint64_t *)P2V(pa), level + 1);
            phys_free_page(pa);                 /* child table page */
        }
        /* user space never contains block descriptors */
    }
}

void process_destroy(process_t *p)
{
    if (!p || p->state == PROC_UNUSED)
        return;

    uint64_t *pgd = p->pgd;

    /* Free only the user portion; kernel entries below USER_L0_FIRST point
     * to shared kernel tables and must not be freed. */
    for (unsigned i = USER_L0_FIRST; i < PTE_PER_TABLE; i++) {
        uint64_t e = pgd[i];
        if ((e & PTE_VALID) && (e & PTE_TABLE)) {
            uintptr_t pa = (uintptr_t)(e & PTE_ADDR_MASK);
            free_subtree((uint64_t *)P2V(pa), 1);
            phys_free_page(pa);                 /* L1 table page */
        }
        pgd[i] = 0;
    }

    phys_free_page(p->pgd_pa);                  /* L0 root */
    asid_free(p->asid);

    p->state  = PROC_UNUSED;
    p->pgd    = (uint64_t *)0;
    p->pgd_pa = 0;
    p->asid   = 0;
}

size_t process_count(void)
{
    size_t n = 0;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (g_proc[i].state != PROC_UNUSED)
            n++;
    return n;
}
