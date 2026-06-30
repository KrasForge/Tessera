/* arch/arm64/pmm.c — Physical frame allocator + BCM2711 memory map (Issue #7)
 *
 * A bitmap allocator over the managed RAM range [PHYS_RAM_START,
 * PHYS_RAM_END).  Bit set = frame in use.  The kernel image, the boot
 * stack, and all low memory below the kernel are reserved at init so the
 * allocator never hands out a page that overlaps running code or data.
 *
 * Ports the allocation idea of IKOS kernel/buddy_allocator.c to a fixed
 * ARM memory map (x86 e820 detection is not applicable on the Pi).
 */

#include "pmm.h"
#include "pmem.h"
#include <stdint.h>
#include <stddef.h>

/* Linker symbols delimiting the loaded kernel image (arch/arm64/kernel.ld).
 * __kernel_end sits above the .bss and boot .stack regions, so reserving
 * [0, __kernel_end) protects code, data, and the active stack.  The host
 * unit test simulates a 1 MiB kernel instead of using linker symbols. */
#ifdef HOSTTEST
#define __kernel_start ((char *)0)
#define __kernel_end   ((char *)0x100000)
#else
extern char __kernel_start[];
extern char __kernel_end[];
#endif

#define PMM_TOTAL_PAGES (PHYS_RAM_END / PAGE_SIZE)          /* 262144 @ 1 GiB */
#define PMM_BITMAP_WORDS (PMM_TOTAL_PAGES / 64)             /* 4096 u64s      */

static uint64_t g_bitmap[PMM_BITMAP_WORDS];
static size_t   g_free_pages;
static size_t   g_first_usable;   /* first allocatable page index */
static size_t   g_next_hint;      /* round-robin search hint      */

static inline void bm_set(size_t i)   { g_bitmap[i >> 6] |=  (1UL << (i & 63)); }
static inline void bm_clear(size_t i) { g_bitmap[i >> 6] &= ~(1UL << (i & 63)); }
static inline int  bm_test(size_t i)  { return (g_bitmap[i >> 6] >> (i & 63)) & 1UL; }

static inline uintptr_t align_up(uintptr_t v, uintptr_t a)
{
    return (v + a - 1) & ~(a - 1);
}

void pmm_init(void)
{
    /* Everything used (1) by default; clear the usable range below. */
    for (size_t w = 0; w < PMM_BITMAP_WORDS; w++)
        g_bitmap[w] = ~0UL;

    /* Reserve [0, __kernel_end): low firmware memory + kernel + stack. */
    uintptr_t kend = align_up((uintptr_t)__kernel_end, PAGE_SIZE);
    g_first_usable = kend / PAGE_SIZE;
    if (g_first_usable >= PMM_TOTAL_PAGES)
        g_first_usable = PMM_TOTAL_PAGES;   /* pathological: no free RAM */

    g_free_pages = 0;
    for (size_t i = g_first_usable; i < PMM_TOTAL_PAGES; i++) {
        bm_clear(i);
        g_free_pages++;
    }
    g_next_hint = g_first_usable;
}

uintptr_t phys_alloc_page(void)
{
    if (g_free_pages == 0)
        return 0;

    for (size_t scan = 0; scan < PMM_TOTAL_PAGES; scan++) {
        size_t i = g_next_hint + scan;
        if (i >= PMM_TOTAL_PAGES)
            i -= (PMM_TOTAL_PAGES - g_first_usable);  /* wrap to usable base */
        if (i < g_first_usable)
            i = g_first_usable;
        if (!bm_test(i)) {
            bm_set(i);
            g_free_pages--;
            g_next_hint = (i + 1 < PMM_TOTAL_PAGES) ? i + 1 : g_first_usable;
            return (uintptr_t)i * PAGE_SIZE;
        }
    }
    return 0;
}

uintptr_t phys_alloc_page_zero(void)
{
    uintptr_t pa = phys_alloc_page();
    if (pa) {
        uint64_t *p = (uint64_t *)P2V(pa);
        for (size_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++)
            p[i] = 0;
    }
    return pa;
}

uintptr_t phys_alloc_contig(size_t n_pages)
{
    if (n_pages == 0)
        return 0;
    if (n_pages == 1)
        return phys_alloc_page();

    /* Linear scan for a run of n_pages clear bits. */
    size_t run = 0, start = g_first_usable;
    for (size_t i = g_first_usable; i < PMM_TOTAL_PAGES; i++) {
        if (!bm_test(i)) {
            if (run == 0)
                start = i;
            if (++run == n_pages) {
                for (size_t j = start; j < start + n_pages; j++)
                    bm_set(j);
                g_free_pages -= n_pages;
                return (uintptr_t)start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

void phys_free_page(uintptr_t pa)
{
    size_t i = pa / PAGE_SIZE;
    if (pa & (PAGE_SIZE - 1))           /* not page-aligned */
        return;
    if (i < g_first_usable || i >= PMM_TOTAL_PAGES)
        return;                          /* outside managed/usable RAM */
    if (!bm_test(i))                     /* double free */
        return;
    bm_clear(i);
    g_free_pages++;
    g_next_hint = i;
}

void phys_free_contig(uintptr_t pa, size_t n_pages)
{
    for (size_t k = 0; k < n_pages; k++)
        phys_free_page(pa + (uintptr_t)k * PAGE_SIZE);
}

size_t pmm_total_pages(void)
{
    return PMM_TOTAL_PAGES - g_first_usable;
}

size_t pmm_free_pages(void)
{
    return g_free_pages;
}

int pmm_is_reserved(uintptr_t pa)
{
    return pa < (uintptr_t)g_first_usable * PAGE_SIZE;
}
