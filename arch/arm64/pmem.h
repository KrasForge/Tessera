/* arch/arm64/pmem.h — physical-frame -> dereferenceable-pointer mapping.
 *
 * On the real ARM target, RAM is identity-mapped, so a physical address is
 * also a valid pointer and P2V() is the identity.  The host unit-test build
 * (-DHOSTTEST) instead backs "physical memory" with a normal heap arena, so
 * P2V() offsets into that arena.  This single indirection lets the same
 * allocator and page-table-walk source run both on bare metal and under a
 * host test harness with AddressSanitizer.
 */

#ifndef ARM64_PMEM_H
#define ARM64_PMEM_H

#include <stdint.h>

#ifdef HOSTTEST
extern unsigned char *g_hosttest_ram;   /* defined by the test harness */
static inline void *P2V(uintptr_t pa) { return (void *)(g_hosttest_ram + pa); }
#else
static inline void *P2V(uintptr_t pa) { return (void *)pa; }  /* identity */
#endif

#endif /* ARM64_PMEM_H */
