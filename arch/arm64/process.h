/* arch/arm64/process.h — per-process address space + PCB (Issue #11, M2)
 *
 * Tessera's isolation model gives every plugin its own virtual address
 * space.  process_create() is where that space is born: it allocates a
 * fresh L0 translation-table root, shares the kernel's mappings into it (so
 * the kernel and MMIO remain reachable), and leaves user space empty.
 *
 * Address-space layout (TTBR0_EL1, 48-bit VA):
 *   L0[0]                  -> kernel identity map (shared, global; from M1)
 *   [USER_VA_BASE, ...)    -> per-process user space (empty at creation)
 *
 * Because the M1 kernel is identity-mapped within L0[0], user space is
 * placed at/above 512 GiB (L0 index 1) so that sharing the kernel is a
 * simple copy of the top-level kernel entries with no aliasing.  A switch to
 * this process installs `ttbr0` (root PA + ASID) into TTBR0_EL1 (issue #15).
 */

#ifndef ARM64_PROCESS_H
#define ARM64_PROCESS_H

#include <stdint.h>
#include <stddef.h>

/* First VA above the kernel's L0[0] entry (512 GiB) and the top of the
 * 48-bit TTBR0 range (exclusive). */
#define USER_VA_BASE  0x0000008000000000UL
#define USER_VA_END   0x0001000000000000UL

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_ZOMBIE,    /* exited normally    */
    PROC_KILLED,    /* terminated by a fault */
} proc_state_t;

/* Process control block.  Holds the physical address of the L0 root and the
 * ASID, per the issue #11 requirement. */
typedef struct process {
    uint32_t     pid;
    uint16_t     asid;
    proc_state_t state;
    uintptr_t    pgd_pa;   /* physical address of the L0 root (TTBR0_EL1)   */
    uint64_t    *pgd;      /* dereferenceable pointer to the L0 root         */
    uint64_t     ttbr0;    /* TTBR0_EL1 value: pgd_pa | (asid << 48)         */
    long         exit_code; /* sys_exit value, or -1 if killed by a fault   */
    char         name[16];
    volatile uint32_t *liveness; /* shared-ring status word, or NULL (#26)   */
} process_t;

/* Value the kernel writes to *liveness when the process is killed; matches
 * ARB_PRODUCER_DEAD in audio_ringbuf.h so the host detects plugin death. */
#define PROC_LIVENESS_DEAD 2u

/* Register a kernel-writable status word (e.g. a shared audio-ring header
 * field) that the kernel sets to PROC_LIVENESS_DEAD when p is killed. */
void process_set_liveness(process_t *p, volatile uint32_t *status);

/* Allocate a process and its empty user address space.  Returns NULL if the
 * process table, ASID space, or physical memory is exhausted. */
process_t *process_create(const char *name);

/* Tear down a process: free every user-space page-table page and mapped user
 * frame, release the L0 root and the ASID.  Shared kernel tables are left
 * untouched. */
void process_destroy(process_t *p);

/* Map [va, va+size) -> [pa, pa+size) into the process user space.  va must
 * lie within [USER_VA_BASE, USER_VA_END).  VMM_USER is added automatically;
 * pass the other VMM_* flags (e.g. VMM_READ | VMM_WRITE).  Returns 0 on
 * success. */
int process_map(process_t *p, uintptr_t pa, uintptr_t va, size_t size,
                unsigned flags);

/* Number of live (non-UNUSED) processes. */
size_t process_count(void);

/* Run process p at EL0 starting at `entry` with stack `user_sp`; `arg0` is
 * passed in the user's x0.  Installs the process address space, runs until
 * the process exits or faults, restores the kernel address space, records
 * the exit code, and sets the state to PROC_ZOMBIE (clean exit) or
 * PROC_KILLED (fault).  Returns the exit code (-1 if killed). */
long process_run(process_t *p, uint64_t entry, uint64_t user_sp, uint64_t arg0);

/* The process currently executing at EL0 (NULL if in the kernel). */
process_t *current_process(void);

/* Set the current process (used by the scheduler on each switch). */
void process_set_current(process_t *p);

#endif /* ARM64_PROCESS_H */
