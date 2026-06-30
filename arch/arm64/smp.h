/* arch/arm64/smp.h - multi-core bring-up and per-CPU state (Issue #21, M4)
 *
 * The CM4 has four Cortex-A72 cores.  Tessera dedicates CPU0 to the audio
 * thread (and the DMA completion interrupt) so nothing can steal its
 * timeslice, and runs the kernel, host process, and plugins on CPU1-3.  This
 * is what backs the README's "guaranteed audio-callback cadence".
 *
 * Secondary cores are started with the PSCI CPU_ON call (the SMC/HVC firmware
 * interface QEMU's 'virt' board and modern Raspberry Pi firmware both
 * implement); each lands in smp_secondary_entry (smp_entry.S), drops to EL1,
 * installs its stack, and calls smp_secondary_main().
 */

#ifndef ARM64_SMP_H
#define ARM64_SMP_H

#include <stdint.h>

#define MAX_CPUS  4

/* Role of a core in the affinity plan. */
typedef enum {
    CPU_ROLE_NONE = 0,
    CPU_ROLE_AUDIO,     /* CPU0: audio thread + DMA IRQ, no other tasks */
    CPU_ROLE_GENERAL,   /* CPU1-3: kernel / host process / plugins      */
} cpu_role_t;

/* Per-CPU control block (one cache-line-ish slot per core). */
typedef struct {
    uint32_t          cpu_id;
    cpu_role_t        role;
    volatile uint32_t online;     /* set by the core once it is running */
    uint64_t          stack_top;  /* SP installed by the trampoline     */
    void            (*entry)(void *);
    void             *arg;
} percpu_t;

/* This core's hardware id (MPIDR_EL1 Aff0), 0..MAX_CPUS-1. */
uint32_t smp_cpu_id(void);

/* The per-CPU block for this core. */
percpu_t *this_cpu(void);

/* The per-CPU block for core `id`. */
percpu_t *smp_cpu(uint32_t id);

/* The affinity plan: CPU0 = audio, CPU1-3 = general. */
cpu_role_t smp_role_for(uint32_t cpu_id);

/* PSCI CPU_ON (64-bit): power up core `target_mpidr` so it begins executing at
 * `entry` with `ctx` in x0.  Returns 0 (PSCI_SUCCESS) or a negative error. */
long psci_cpu_on(uint64_t target_mpidr, uint64_t entry, uint64_t ctx);

/* Start secondary core `cpu_id` running `fn(arg)` on a stack whose top is
 * `stack_top`.  Returns 0 on success.  Must run on the primary (CPU0). */
int smp_start_core(uint32_t cpu_id, void (*fn)(void *), void *arg,
                   uint64_t stack_top);

/* Assembly trampoline (smp_entry.S) and the C landing it calls. */
void smp_secondary_entry(void);
void smp_secondary_main(void);

#endif /* ARM64_SMP_H */
