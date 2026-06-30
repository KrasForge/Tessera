/* arch/arm64/kheap.c — Kernel heap: kalloc / kfree (Issue #10)
 *
 * First-fit free-list allocator over a single contiguous arena of physical
 * frames obtained from the PMM.  Because RAM is identity-mapped, the arena
 * is addressed directly by its physical address.
 *
 * Each block carries a header recording its payload size, a free flag, and
 * a magic value.  Blocks form an address-ordered doubly linked list; the
 * arena is always fully covered (no gaps), so coalescing simply merges a
 * freed block with its immediate neighbours when they are also free.
 *
 * Ports the kernel-heap role of IKOS kernel/kalloc.c onto the ARM physical
 * allocator from issue #7.
 */

#include "kheap.h"
#include "pmm.h"
#include "pmem.h"
#include <stdint.h>
#include <stddef.h>

#define KHEAP_PAGES     512u            /* 2 MiB arena */
#define KHEAP_ALIGN     16u
#define HDR_MAGIC       0x4845415048ULL /* "HEAPH" */
#define MIN_SPLIT       32u             /* don't split off slivers smaller   */

/* 48-byte header (a multiple of KHEAP_ALIGN) so that a payload immediately
 * following a 16-byte-aligned block start is itself 16-byte aligned. */
typedef struct hdr {
    uint64_t     magic;
    size_t       size;      /* payload bytes (excludes this header) */
    struct hdr  *prev;      /* previous block in address order      */
    struct hdr  *next;      /* next block in address order          */
    uint32_t     free;      /* 1 = free, 0 = allocated              */
    uint32_t     _pad0;
    uint64_t     _pad1;
} hdr_t;

static hdr_t  *g_heap;          /* lowest-address block */
static size_t  g_used_bytes;

static inline void *payload_of(hdr_t *h) { return (void *)(h + 1); }
static inline hdr_t *hdr_of(void *p)     { return (hdr_t *)p - 1; }

static inline size_t round_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

void kheap_init(void)
{
    uintptr_t base = phys_alloc_contig(KHEAP_PAGES);
    if (!base) {
        g_heap = (hdr_t *)0;
        return;
    }
    g_heap = (hdr_t *)P2V(base);
    g_heap->magic = HDR_MAGIC;
    g_heap->size  = (size_t)KHEAP_PAGES * PAGE_SIZE - sizeof(hdr_t);
    g_heap->free  = 1;
    g_heap->prev  = (hdr_t *)0;
    g_heap->next  = (hdr_t *)0;
    g_used_bytes  = 0;
}

void *kalloc(size_t size)
{
    if (size == 0 || g_heap == (hdr_t *)0)
        return (void *)0;

    size_t need = round_up(size, KHEAP_ALIGN);

    for (hdr_t *h = g_heap; h; h = h->next) {
        if (!h->free || h->size < need)
            continue;

        /* Split off the remainder if it is large enough to be useful. */
        if (h->size >= need + sizeof(hdr_t) + MIN_SPLIT) {
            hdr_t *rest = (hdr_t *)((uint8_t *)payload_of(h) + need);
            rest->magic = HDR_MAGIC;
            rest->size  = h->size - need - sizeof(hdr_t);
            rest->free  = 1;
            rest->prev  = h;
            rest->next  = h->next;
            if (rest->next)
                rest->next->prev = rest;
            h->next = rest;
            h->size = need;
        }

        h->free = 0;
        g_used_bytes += h->size;
        return payload_of(h);
    }
    return (void *)0;   /* out of memory */
}

void *kzalloc(size_t size)
{
    void *p = kalloc(size);
    if (p) {
        uint8_t *b = p;
        size_t n = hdr_of(p)->size;
        for (size_t i = 0; i < n; i++)
            b[i] = 0;
    }
    return p;
}

/* Merge h with h->next when both are free (and, by arena invariant,
 * physically adjacent). */
static void coalesce_forward(hdr_t *h)
{
    hdr_t *n = h->next;
    if (!n || !n->free)
        return;
    h->size += sizeof(hdr_t) + n->size;
    h->next  = n->next;
    if (n->next)
        n->next->prev = h;
    n->magic = 0;   /* invalidate the absorbed header */
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    hdr_t *h = hdr_of(ptr);
    if (h->magic != HDR_MAGIC || h->free) {
        /* Double free or wild pointer — report and ignore. */
        extern void uart_puts(const char *);
        uart_puts("kheap: invalid/double free detected\r\n");
        return;
    }

    h->free = 1;
    g_used_bytes -= h->size;

    coalesce_forward(h);
    if (h->prev && h->prev->free)
        coalesce_forward(h->prev);
}

size_t kheap_used(void)
{
    return g_used_bytes;
}
