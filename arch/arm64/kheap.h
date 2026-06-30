/* arch/arm64/kheap.h — Kernel heap (kalloc / kfree), Issue #10
 *
 * A first-fit free-list allocator backed by a contiguous arena of physical
 * frames from the PMM (identity-mapped, so the arena is directly
 * addressable).  Provides the general-purpose kernel heap used above the
 * page granularity offered by phys_alloc_page().
 */

#ifndef ARM64_KHEAP_H
#define ARM64_KHEAP_H

#include <stddef.h>

/* Reserve and initialise the kernel heap arena.  pmm_init() must have run. */
void kheap_init(void);

/* Allocate at least size bytes, 16-byte aligned.  NULL on out-of-memory. */
void *kalloc(size_t size);

/* Allocate and zero size bytes. */
void *kzalloc(size_t size);

/* Free a pointer returned by kalloc/kzalloc.  Double frees are detected
 * (a diagnostic is printed and the call is ignored). */
void kfree(void *ptr);

/* Currently-allocated payload bytes (excludes per-block headers). */
size_t kheap_used(void);

#endif /* ARM64_KHEAP_H */
