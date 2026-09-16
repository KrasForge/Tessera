/* Per-core EL0 return state. Targets currently have four Aff0 CPU IDs. */
#ifndef ARM64_EL0_CONTEXT_H
#define ARM64_EL0_CONTEXT_H
#define EL0_CONTEXT_CPUS 4
#define EL0_RESUME_BYTES 128
#ifndef __ASSEMBLER__
#include <stdint.h>
static inline uint32_t el0_cpu_index(void)
{
#if defined(__aarch64__) && !defined(HOSTTEST)
    uint64_t id;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(id));
    return (uint32_t)id & (EL0_CONTEXT_CPUS - 1u);
#else
    return 0;
#endif
}
extern unsigned char g_kresume[EL0_CONTEXT_CPUS][EL0_RESUME_BYTES];
extern uint64_t g_user_spsr[EL0_CONTEXT_CPUS];
#endif
#endif
