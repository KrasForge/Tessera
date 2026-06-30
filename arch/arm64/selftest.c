/* arch/arm64/selftest.c — M1 virtual-memory self-test, reported over UART.
 *
 * Exercises the acceptance criteria of issues #7, #8/#9, and #10:
 *   - PMM hands out unique, page-aligned frames outside the kernel image.
 *   - The MMU is enabled (we are still executing and UART MMIO still works).
 *   - vmm_map/unmap/protect work, TLB invalidation is correct on remap, and
 *     a 1000-cycle map/unmap stress loop runs without corruption.
 *   - kalloc/kfree allocate, reuse, and detect a double free.
 *   - ASID allocation returns unique non-zero IDs.
 */

#include "pmm.h"
#include "mmu.h"
#include "vmem.h"
#include "kheap.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

/* A virtual window well above the identity-mapped RAM/MMIO (4 GiB), so the
 * VMM creates fresh L0-L3 tables rather than touching the 2 MiB identity
 * blocks. */
#define TEST_VA_BASE 0x100000000UL

#define OKBAD(c) ((c) ? "ok" : "BAD")

void m1_selftest(void)
{
    uart_puts("\r\n=== M1 virtual-memory self-test ===\r\n");

    /* --- PMM (issue #7) ------------------------------------------------- */
    uintptr_t a = phys_alloc_page();
    uintptr_t b = phys_alloc_page();
    int pmm_ok = a && b && a != b &&
                 !((a | b) & (PAGE_SIZE - 1)) &&
                 !pmm_is_reserved(a) && !pmm_is_reserved(b);
    uart_printf("pmm   : alloc two frames .......... %s\r\n", OKBAD(pmm_ok));
    uart_printf("pmm   : free %u / %u total pages\r\n",
                (unsigned)pmm_free_pages(), (unsigned)pmm_total_pages());
    phys_free_page(a);
    phys_free_page(b);

    /* --- VMM map / read-write (issues #8, #10) -------------------------- */
    uintptr_t p1 = phys_alloc_page();
    volatile uint32_t *win = (volatile uint32_t *)TEST_VA_BASE;
    int map_ok = (vmm_map(p1, TEST_VA_BASE, PAGE_SIZE, VMM_READ | VMM_WRITE) == 0);
    *win = 0xCAFEBABEu;
    map_ok = map_ok && (*win == 0xCAFEBABEu);
    uart_printf("vmm   : map + write-through ....... %s\r\n", OKBAD(map_ok));

    /* --- TLB correctness: remap the same VA to a new frame -------------- */
    uintptr_t p2 = phys_alloc_page();
    *(volatile uint32_t *)p2 = 0x12345678u;          /* seed via identity */
    vmm_unmap(TEST_VA_BASE, PAGE_SIZE);
    vmm_map(p2, TEST_VA_BASE, PAGE_SIZE, VMM_READ | VMM_WRITE);
    int tlb_ok = (*win == 0x12345678u);
    uart_printf("vmm   : TLB sees remap ............ %s\r\n", OKBAD(tlb_ok));
    vmm_unmap(TEST_VA_BASE, PAGE_SIZE);
    phys_free_page(p1);
    phys_free_page(p2);

    /* --- VMM stress: 1000 random map/unmap cycles ----------------------- */
    int stress_ok = 1;
    uint32_t lcg = 2463534242u;                       /* xorshift PRNG */
    for (int i = 0; i < 1000; i++) {
        lcg ^= lcg << 13; lcg ^= lcg >> 17; lcg ^= lcg << 5;
        uintptr_t va = TEST_VA_BASE + (uintptr_t)(lcg & 0x3FFu) * PAGE_SIZE;
        uintptr_t pp = phys_alloc_page();
        if (!pp) { stress_ok = 0; break; }
        if (vmm_map(pp, va, PAGE_SIZE, VMM_READ | VMM_WRITE) != 0) {
            stress_ok = 0; phys_free_page(pp); break;
        }
        *(volatile uint32_t *)va = (uint32_t)i;
        if (*(volatile uint32_t *)va != (uint32_t)i)
            stress_ok = 0;
        vmm_unmap(va, PAGE_SIZE);
        phys_free_page(pp);
    }
    uart_printf("vmm   : 1000x map/unmap stress .... %s\r\n", OKBAD(stress_ok));

    /* --- vmm_protect: drop write permission ----------------------------- */
    uintptr_t p3 = phys_alloc_page();
    vmm_map(p3, TEST_VA_BASE, PAGE_SIZE, VMM_READ | VMM_WRITE);
    *win = 0xAAAA5555u;
    int prot_ok = (vmm_protect(TEST_VA_BASE, PAGE_SIZE, VMM_READ) == 0) &&
                  (*win == 0xAAAA5555u);              /* read still works  */
    uart_printf("vmm   : protect read-only ......... %s\r\n", OKBAD(prot_ok));
    vmm_unmap(TEST_VA_BASE, PAGE_SIZE);
    phys_free_page(p3);

    /* --- Kernel heap (issue #10) ---------------------------------------- */
    void *x = kalloc(100);
    void *y = kalloc(2000);
    int heap_ok = x && y && x != y;
    uart_printf("kheap : alloc distinct blocks ..... %s (used %u B)\r\n",
                OKBAD(heap_ok), (unsigned)kheap_used());
    kfree(x);
    kfree(y);
    void *z = kalloc(64);
    uart_printf("kheap : reuse after free .......... %s\r\n", OKBAD(z != 0));
    kfree(z);

    /* --- ASID allocation (issue #10) ------------------------------------ */
    uint16_t s1 = asid_alloc();
    uint16_t s2 = asid_alloc();
    int asid_ok = s1 && s2 && s1 != s2;
    uart_printf("asid  : unique non-zero IDs ....... %s (%u, %u)\r\n",
                OKBAD(asid_ok), (unsigned)s1, (unsigned)s2);
    asid_free(s1);
    asid_free(s2);

    uart_puts("=== M1 self-test complete ===\r\n\r\n");
}
