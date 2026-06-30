/* tests/arm64/m1_host_test.c — host unit tests for the M1 memory subsystem.
 *
 * Compiles the real arch/arm64 sources (pmm.c, mmu.c, vmem.c, kheap.c) with
 * -DHOSTTEST so that "physical memory" is backed by a host arena (see
 * pmem.h) and the privileged MMU/TLB instructions become no-ops.  The
 * page-table *logic* — descriptor encoding, multi-level walk, map/unmap/
 * protect, ASID and heap management — is exactly the code that runs on the
 * Pi, so this exercises the issue #7/#8/#10 acceptance criteria on any host.
 *
 * Build/run via:  make test-arm-m1
 */

/* _GNU_SOURCE exposes MAP_ANONYMOUS / MAP_NORESERVE under strict -std=c11. */
#define _GNU_SOURCE

#include "pmm.h"
#include "mmu.h"
#include "vmem.h"
#include "kheap.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

/* Backing store for simulated physical RAM (see arch/arm64/pmem.h). */
unsigned char *g_hosttest_ram;

/* kheap.c references uart_puts for its double-free diagnostic. */
void uart_puts(const char *s) { fputs(s, stdout); }

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* The test VA window, matching arch/arm64/selftest.c. */
#define TEST_VA 0x100000000UL

/* Decode the L3 descriptor backing va and sanity-check it against the
 * physical address and flags it was mapped with. */
static uint64_t l3_entry(uintptr_t va)
{
    uint64_t *e = mmu_walk_l3(mmu_kernel_pgd(), va, 0);
    return e ? *e : 0;
}

static void test_pmm(void)
{
    printf("[pmm] physical frame allocator (issue #7)\n");
    pmm_init();

    size_t total = pmm_total_pages();
    size_t free0 = pmm_free_pages();
    CHECK(total > 0 && free0 == total, "all managed frames free after init");

    /* Allocate many frames; verify alignment, uniqueness, non-reserved. */
    enum { N = 4096 };
    static uintptr_t pages[N];
    int aligned = 1, nonreserved = 1;
    for (int i = 0; i < N; i++) {
        pages[i] = phys_alloc_page();
        if (pages[i] & (PAGE_SIZE - 1)) aligned = 0;
        if (pmm_is_reserved(pages[i]))  nonreserved = 0;
    }
    CHECK(aligned, "every frame is 4 KiB aligned");
    CHECK(nonreserved, "no frame lies in the reserved kernel region");

    int unique = 1;
    for (int i = 1; i < N; i++)
        if (pages[i] == pages[i - 1]) unique = 0;   /* sequential alloc */
    /* Thorough uniqueness: a frame must not repeat — check via a marker. */
    for (int i = 0; i < N; i++)
        *(uint32_t *)((unsigned char *)g_hosttest_ram + pages[i]) = (uint32_t)i;
    for (int i = 0; i < N; i++)
        if (*(uint32_t *)((unsigned char *)g_hosttest_ram + pages[i]) != (uint32_t)i)
            unique = 0;
    CHECK(unique, "4096 frames are mutually distinct");

    CHECK(pmm_free_pages() == free0 - N, "free count dropped by N");
    for (int i = 0; i < N; i++)
        phys_free_page(pages[i]);
    CHECK(pmm_free_pages() == free0, "free count restored after release");

    /* Contiguous allocation. */
    uintptr_t c = phys_alloc_contig(64);
    int contig_ok = c != 0 && !(c & (PAGE_SIZE - 1));
    CHECK(contig_ok, "phys_alloc_contig(64) returns aligned run");
    phys_free_contig(c, 64);
}

static void test_mmu_vmm(void)
{
    printf("[vmm] translation tables + map/unmap/protect (issues #8/#10)\n");
    pmm_init();
    mmu_init();
    CHECK(mmu_kernel_pgd() != NULL, "kernel page-table root built");

    uintptr_t pa = phys_alloc_page();
    CHECK(vmm_map(pa, TEST_VA, PAGE_SIZE, VMM_READ | VMM_WRITE) == 0,
          "vmm_map RW succeeds");

    uint64_t e = l3_entry(TEST_VA);
    CHECK((e & PTE_VALID) && (e & PTE_PAGE), "L3 entry valid + page");
    CHECK((e & PTE_ADDR_MASK) == (pa & PTE_ADDR_MASK), "output address matches PA");
    CHECK(((e >> 2) & 7) == MAIR_IDX_NORMAL, "Normal memory AttrIndx");
    CHECK((e & PTE_AF), "access flag set");
    CHECK(((e >> 6) & 3) == 0 /*RW EL1*/, "kernel RW access permission");
    CHECK((e & PTE_PXN) == 0 ? 0 : 1, "kernel data is privileged-execute-never");

    /* protect -> read only: AP changes to RO_EL1 (0b10). */
    CHECK(vmm_protect(TEST_VA, PAGE_SIZE, VMM_READ) == 0, "vmm_protect RO succeeds");
    e = l3_entry(TEST_VA);
    CHECK(((e >> 6) & 3) == 2, "permission downgraded to read-only");
    CHECK((e & PTE_ADDR_MASK) == (pa & PTE_ADDR_MASK), "protect preserves PA");

    /* unmap clears the descriptor. */
    CHECK(vmm_unmap(TEST_VA, PAGE_SIZE) == 0, "vmm_unmap succeeds");
    CHECK((l3_entry(TEST_VA) & PTE_VALID) == 0, "descriptor cleared after unmap");
    phys_free_page(pa);

    /* Device mapping: AttrIndx -> device, execute-never both levels. */
    uintptr_t dpa = phys_alloc_page();
    vmm_map(dpa, TEST_VA, PAGE_SIZE, VMM_READ | VMM_WRITE | VMM_DEVICE);
    e = l3_entry(TEST_VA);
    CHECK(((e >> 2) & 7) == MAIR_IDX_DEVICE, "device AttrIndx for MMIO flag");
    CHECK((e & PTE_PXN) && (e & PTE_UXN), "device mapping is execute-never");
    vmm_unmap(TEST_VA, PAGE_SIZE);
    phys_free_page(dpa);

    /* User + exec mapping: AP user, not-global, UXN clear (EL0 may exec). */
    uintptr_t upa = phys_alloc_page();
    vmm_map(upa, TEST_VA, PAGE_SIZE, VMM_READ | VMM_WRITE | VMM_USER | VMM_EXEC);
    e = l3_entry(TEST_VA);
    CHECK(((e >> 6) & 3) == 1, "user RW access permission");
    CHECK((e & PTE_NG), "user mapping is ASID-tagged (not-global)");
    CHECK((e & PTE_UXN) == 0, "user-executable: UXN clear");
    CHECK((e & PTE_PXN), "user page is privileged-execute-never");
    vmm_unmap(TEST_VA, PAGE_SIZE);
    phys_free_page(upa);
}

static void test_vmm_stress(void)
{
    printf("[vmm] 1000x random map/unmap stress (issue #10)\n");
    pmm_init();
    mmu_init();

    int ok = 1;
    uint32_t lcg = 2463534242u;
    for (int i = 0; i < 1000; i++) {
        lcg ^= lcg << 13; lcg ^= lcg >> 17; lcg ^= lcg << 5;
        uintptr_t va = TEST_VA + (uintptr_t)(lcg & 0x3FFu) * PAGE_SIZE;
        uintptr_t pp = phys_alloc_page();
        if (!pp) { ok = 0; break; }
        if (vmm_map(pp, va, PAGE_SIZE, VMM_READ | VMM_WRITE) != 0) { ok = 0; break; }
        uint64_t e = l3_entry(va);
        if (!(e & PTE_VALID) || (e & PTE_ADDR_MASK) != (pp & PTE_ADDR_MASK))
            ok = 0;
        vmm_unmap(va, PAGE_SIZE);
        if (l3_entry(va) & PTE_VALID) ok = 0;
        phys_free_page(pp);
    }
    CHECK(ok, "1000 map/verify/unmap cycles without corruption");
}

static void test_kheap(void)
{
    printf("[kheap] kalloc/kfree first-fit allocator (issue #10)\n");
    pmm_init();
    kheap_init();

    void *a = kalloc(100);
    void *b = kalloc(2000);
    CHECK(a && b && a != b, "distinct allocations");
    CHECK(((uintptr_t)a % 16) == 0 && ((uintptr_t)b % 16) == 0,
          "pointers are 16-byte aligned");
    kfree(a);
    kfree(b);
    void *c = kalloc(64);
    CHECK(c != NULL, "allocation succeeds after free (reuse)");
    kfree(c);
    CHECK(kheap_used() == 0, "heap fully accounted after frees");

    /* Fuzz: random alloc/free with per-block canaries to catch overlap. */
    enum { SLOTS = 256, ITERS = 20000 };
    static void  *ptr[SLOTS];
    static size_t len[SLOTS];
    uint32_t rng = 12345u;
    int overlap = 0, oom = 0;
    for (int it = 0; it < ITERS; it++) {
        rng = rng * 1103515245u + 12345u;
        int s = (rng >> 8) % SLOTS;
        if (ptr[s]) {
            /* verify canary then free */
            unsigned char *p = ptr[s];
            for (size_t k = 0; k < len[s]; k++)
                if (p[k] != (unsigned char)((uintptr_t)p + k)) { overlap = 1; break; }
            kfree(ptr[s]);
            ptr[s] = NULL;
        } else {
            size_t sz = 1 + ((rng >> 16) % 512);
            unsigned char *p = kalloc(sz);
            if (!p) { oom = 1; continue; }
            for (size_t k = 0; k < sz; k++)
                p[k] = (unsigned char)((uintptr_t)p + k);
            ptr[s] = p;
            len[s] = sz;
        }
    }
    for (int s = 0; s < SLOTS; s++)
        if (ptr[s]) {
            unsigned char *p = ptr[s];
            for (size_t k = 0; k < len[s]; k++)
                if (p[k] != (unsigned char)((uintptr_t)p + k)) overlap = 1;
            kfree(ptr[s]);
        }
    CHECK(!overlap, "20000-op fuzz: no canary corruption / block overlap");
    CHECK(!oom, "fuzz did not exhaust the heap");
    CHECK(kheap_used() == 0, "heap balanced after fuzz");
}

static void test_asid(void)
{
    printf("[asid] address-space ID allocator (issue #10)\n");
    uint16_t first = asid_alloc();
    uint16_t second = asid_alloc();
    CHECK(first != 0 && second != 0 && first != second, "unique non-zero ASIDs");

    /* Exhaust then confirm recycling. */
    asid_free(first);
    uint16_t again = asid_alloc();
    CHECK(again == first, "freed ASID is recycled");
    asid_free(second);
    asid_free(again);

    uint64_t ttbr = vmm_make_ttbr(0x1234000, 7);
    CHECK((ttbr >> 48) == 7 && (ttbr & PTE_ADDR_MASK) == 0x1234000,
          "TTBR packs ASID in [63:48] and table PA in [47:12]");
}

int main(void)
{
    /* Lazily-backed arena covering the full managed PA range. */
    g_hosttest_ram = mmap(NULL, PHYS_RAM_END, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_hosttest_ram == MAP_FAILED) {
        perror("mmap");
        return 2;
    }

    printf("=== Tessera M1 host unit tests ===\n");
    test_pmm();
    test_mmu_vmm();
    test_vmm_stress();
    test_kheap();
    test_asid();

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
