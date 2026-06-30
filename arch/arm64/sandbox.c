/* arch/arm64/sandbox.c - plugin sandbox audit (Issue #35, M8) */

#include "sandbox.h"
#include "mmu.h"
#include "pmem.h"
#include <stdint.h>
#include <stddef.h>

#define PTE_PER_TABLE 512
/* L0 index of the first user entry; everything below is shared kernel. */
#define SANDBOX_USER_L0_FIRST (USER_VA_BASE >> 39)

int sandbox_va_allowed(uint64_t va, const sandbox_region_t *allow, int n)
{
    for (int i = 0; i < n; i++)
        if (va >= allow[i].va && va < allow[i].va + allow[i].len)
            return 1;
    return 0;
}

void sandbox_classify_pte(uint64_t pte, int *device, int *wx)
{
    /* AttrIndx -> MAIR slot: device/MMIO memory uses MAIR_IDX_DEVICE. */
    unsigned attridx = (unsigned)((pte >> 2) & 7);
    int dev = (attridx == MAIR_IDX_DEVICE);

    /* EL0 access permissions live in AP[2:1] (bits 7:6): RW_ALL (01) is
     * writable at EL0; UXN clear means executable at EL0. */
    unsigned ap = (unsigned)((pte >> 6) & 3);
    int el0_writable   = (ap == 1);                 /* PTE_AP_RW_ALL */
    int el0_executable = ((pte & PTE_UXN) == 0);

    if (device) *device = dev;
    if (wx)     *wx     = (el0_writable && el0_executable);
}

/* Recurse a translation table, classifying every present leaf.  `level` is the
 * level of `table` (1=L1, 2=L2, 3=L3); `base` is the VA mapped by table[0]. */
static void walk(const uint64_t *table, int level, uint64_t base,
                 const sandbox_region_t *allow, int n_allow,
                 sandbox_report_t *r)
{
    uint64_t stride = 1ull << (39 - 9 * level);   /* L1:1G L2:2M L3:4K */

    for (int i = 0; i < PTE_PER_TABLE; i++) {
        uint64_t e = table[i];
        if (!(e & PTE_VALID))
            continue;
        uint64_t va = base + (uint64_t)i * stride;
        uintptr_t pa = (uintptr_t)(e & PTE_ADDR_MASK);

        if (level < 3 && (e & PTE_TABLE)) {
            walk((const uint64_t *)P2V(pa), level + 1, va, allow, n_allow, r);
            continue;
        }

        /* A leaf (an L3 page, or a block at L1/L2 - which user space never
         * uses, but classify it conservatively as one mapping anyway). */
        r->total_pages++;
        int device = 0, wx = 0;
        sandbox_classify_pte(e, &device, &wx);
        int outside = !sandbox_va_allowed(va, allow, n_allow);
        if (outside)  r->outside_pages++;
        if (device)   r->device_pages++;
        if (wx)       r->wx_pages++;
        if ((outside || device || wx) && r->first_bad_va == 0)
            r->first_bad_va = va;
    }
}

int sandbox_audit(const process_t *p, const sandbox_region_t *allow,
                  int n_allow, sandbox_report_t *r)
{
    sandbox_report_t local = {0, 0, 0, 0, 0};
    if (!r) r = &local;
    r->total_pages = r->outside_pages = r->device_pages = r->wx_pages = 0;
    r->first_bad_va = 0;

    if (!p || !p->pgd)
        return -1;

    /* Only the user portion (L0[1..]); L0[0] is the shared kernel map, which
     * is EL1-only and global and deliberately not part of the sandbox. */
    for (int i = (int)SANDBOX_USER_L0_FIRST; i < PTE_PER_TABLE; i++) {
        uint64_t e = p->pgd[i];
        if (!(e & PTE_VALID) || !(e & PTE_TABLE))
            continue;
        uintptr_t pa = (uintptr_t)(e & PTE_ADDR_MASK);
        walk((const uint64_t *)P2V(pa), 1, (uint64_t)i << 39, allow, n_allow, r);
    }

    int violations = r->outside_pages + r->device_pages + r->wx_pages;
    return violations ? -violations : 0;
}
