/* arch/arm64/smp.c - multi-core bring-up and per-CPU state (Issue #21, M4) */

#include "smp.h"
#include <stdint.h>

/* PSCI v0.2 function IDs (SMC Calling Convention, 64-bit). */
#define PSCI_CPU_ON_AARCH64  0xC4000003UL

static percpu_t g_percpu[MAX_CPUS];

/* Stack tops handed to secondary cores, indexed by cpu id; read by the
 * assembly trampoline (smp_entry.S). */
uint64_t g_smp_stack[MAX_CPUS];

uint32_t smp_cpu_id(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFFu);
}

percpu_t *smp_cpu(uint32_t id)
{
    return (id < MAX_CPUS) ? &g_percpu[id] : (percpu_t *)0;
}

percpu_t *this_cpu(void)
{
    return smp_cpu(smp_cpu_id());
}

cpu_role_t smp_role_for(uint32_t cpu_id)
{
    return (cpu_id == 0) ? CPU_ROLE_AUDIO : CPU_ROLE_GENERAL;
}

long psci_cpu_on(uint64_t target_mpidr, uint64_t entry, uint64_t ctx)
{
    register uint64_t x0 __asm__("x0") = PSCI_CPU_ON_AARCH64;
    register uint64_t x1 __asm__("x1") = target_mpidr;
    register uint64_t x2 __asm__("x2") = entry;
    register uint64_t x3 __asm__("x3") = ctx;

    /* QEMU 'virt' (and the Pi firmware) expose PSCI over HVC when the kernel
     * runs at EL1 below an EL2 PSCI implementation. */
    __asm__ volatile("hvc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return (long)x0;
}

int smp_start_core(uint32_t cpu_id, void (*fn)(void *), void *arg,
                   uint64_t stack_top)
{
    if (cpu_id == 0 || cpu_id >= MAX_CPUS)
        return -1;

    percpu_t *c = &g_percpu[cpu_id];
    c->cpu_id    = cpu_id;
    c->role      = smp_role_for(cpu_id);
    c->online    = 0;
    c->stack_top = stack_top;
    c->entry     = fn;
    c->arg       = arg;
    g_smp_stack[cpu_id] = stack_top;

    /* Publish the per-CPU block before the core can read it. */
    __asm__ volatile("dsb sy" ::: "memory");

    long r = psci_cpu_on((uint64_t)cpu_id,
                         (uint64_t)(uintptr_t)smp_secondary_entry,
                         (uint64_t)cpu_id);
    return (r == 0) ? 0 : -1;
}

/* C landing for a freshly-woken secondary core (called from smp_entry.S after
 * the EL2->EL1 drop and stack setup).  Marks the core online and runs its
 * assigned function; if none, it parks. */
void smp_secondary_main(void)
{
    percpu_t *c = this_cpu();
    if (!c)
        for (;;)
            __asm__ volatile("wfe");

    c->cpu_id = smp_cpu_id();
    c->role   = smp_role_for(c->cpu_id);
    __atomic_store_n(&c->online, 1u, __ATOMIC_RELEASE);

    if (c->entry)
        c->entry(c->arg);

    for (;;)
        __asm__ volatile("wfe");
}
