/* arch/arm64/vmem.h — Virtual memory API: map/unmap/protect, TLB, ASID
 *                      (Issue #10)
 *
 * Stable interface used by every subsystem above the hardware layer to
 * manage virtual mappings in the kernel address space, built on the
 * ARMv8 translation-table format from mmu.c (issue #8).
 */

#ifndef ARM64_VMEM_H
#define ARM64_VMEM_H

#include <stdint.h>
#include <stddef.h>

/* Permission / attribute flags for vmm_map() and vmm_protect(). */
#define VMM_READ    0x01u   /* Readable (implied by a valid mapping)        */
#define VMM_WRITE   0x02u   /* Writable                                     */
#define VMM_EXEC    0x04u   /* Executable (clears the relevant XN bit)      */
#define VMM_USER    0x08u   /* Accessible from EL0; mapping is ASID-tagged  */
#define VMM_DEVICE  0x10u   /* Device-nGnRE memory (MMIO) instead of Normal */

/* Map [va, va+size) -> [pa, pa+size) in the kernel page tables.
 * va, pa and size must be 4 KiB aligned.  Returns 0 on success. */
int vmm_map(uintptr_t pa, uintptr_t va, size_t size, unsigned flags);

/* Remove the mapping for [va, va+size).  Returns 0 on success. */
int vmm_unmap(uintptr_t va, size_t size);

/* Change the permissions of an existing mapping, preserving its physical
 * backing.  Returns 0 on success, -1 if any page is not mapped. */
int vmm_protect(uintptr_t va, size_t size, unsigned flags);

/* Flush the entire EL1 TLB (inner-shareable). */
void tlb_flush_all(void);

/* ----------------------------------------------------------------------- *
 * ASID management — each process address space gets a unique 8-bit ASID
 * carried in TTBR0_EL1[63:48] so that context switches avoid a full TLB
 * flush.  ASID 0 is reserved for the global kernel mapping.
 * ----------------------------------------------------------------------- */
uint16_t asid_alloc(void);          /* 1..255, or 0 if exhausted */
void     asid_free(uint16_t asid);

/* Compose a TTBR0_EL1 value from a table physical address and an ASID. */
uint64_t vmm_make_ttbr(uintptr_t table_pa, uint16_t asid);

#endif /* ARM64_VMEM_H */
