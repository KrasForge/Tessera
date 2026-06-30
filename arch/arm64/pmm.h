/* arch/arm64/pmm.h — Physical frame allocator + BCM2711 memory map (Issue #7)
 *
 * The MMU and every process address space are built out of 4 KiB physical
 * frames handed out by this allocator.  It is the lowest layer of the
 * Tessera ARM memory subsystem and must be initialised before mmu_init().
 */

#ifndef ARM64_PMM_H
#define ARM64_PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE   4096UL
#define PAGE_SHIFT  12

/* ----------------------------------------------------------------------- *
 * BCM2711 physical memory map (Raspberry Pi CM4 / Pi 4)
 *
 * RAM starts at 0x0.  The CM4 ships with 1-8 GiB of SDRAM; QEMU's raspi4b
 * model defaults to 1 GiB.  We manage the first 1 GiB, which covers the
 * QEMU default and is sufficient for the kernel + page tables + heap.
 *
 * The low-peripheral MMIO window (UART, GPIO, mailbox, …) lives at
 * 0xFC000000-0xFFFFFFFF in BCM2711 "low peripheral" mode.
 * ----------------------------------------------------------------------- */
/* Defaults are the BCM2711 (Raspberry Pi) map.  They can be overridden at
 * build time (e.g. -DPHYS_RAM_START=...) so the same allocator and MMU code
 * can run on other boards such as the QEMU 'virt' machine in the fault
 * containment harness. */
#ifndef PHYS_RAM_START
#define PHYS_RAM_START  0x00000000UL
#endif
#ifndef PHYS_RAM_END
#define PHYS_RAM_END    0x40000000UL   /* 1 GiB managed RAM */
#endif
#ifndef MMIO_BASE
#define MMIO_BASE       0xFC000000UL
#endif
#ifndef MMIO_END
#define MMIO_END        0x100000000UL  /* exclusive: top of 32-bit space */
#endif

/* Initialise the allocator: mark the kernel image, the boot stack, and all
 * low memory below the kernel as reserved; the remainder of RAM is free. */
void pmm_init(void);

/* Allocate one 4 KiB physical frame.  Returns the physical address, or 0
 * if no free frame is available. */
uintptr_t phys_alloc_page(void);

/* Allocate one frame and zero its contents.  Returns 0 on failure. */
uintptr_t phys_alloc_page_zero(void);

/* Allocate n physically-contiguous 4 KiB frames.  Returns the base
 * physical address, or 0 if no run of n free frames exists. */
uintptr_t phys_alloc_contig(size_t n_pages);

/* Release a frame previously returned by phys_alloc_page(). */
void phys_free_page(uintptr_t pa);

/* Release a contiguous run previously returned by phys_alloc_contig(). */
void phys_free_contig(uintptr_t pa, size_t n_pages);

/* Statistics. */
size_t pmm_total_pages(void);
size_t pmm_free_pages(void);

/* True if pa falls inside the reserved kernel image / low-memory region. */
int pmm_is_reserved(uintptr_t pa);

#endif /* ARM64_PMM_H */
