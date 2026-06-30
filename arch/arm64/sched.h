/* arch/arm64/sched.h - cooperative round-robin scheduler (Issue #15, M2)
 *
 * A minimal cooperative scheduler over EL0 processes.  Each task yields with
 * sys_yield; the kernel saves its EL0 register frame, switches the address
 * space (switch_mm), and resumes the next task.  This is the ARM-port stand
 * in for kernel/scheduler.c until the preemptive timer arrives in M4.
 */

#ifndef ARM64_SCHED_H
#define ARM64_SCHED_H

#include "process.h"
#include <stdint.h>

/* Kernel-thread context saved by cpu_switch() (context_switch.S). */
struct cpu_context {
    uint64_t regs[12];   /* x19..x30 */
    uint64_t sp;
};
void cpu_switch(struct cpu_context *prev, struct cpu_context *next);

void sched_init(void);

/* Add an EL0 task: process p starts at `entry` with stack `user_sp`, and
 * `arg0` in its x0.  Returns the task index, or -1 if the queue is full. */
int sched_add(process_t *p, uint64_t entry, uint64_t user_sp, uint64_t arg0);

/* Run the round-robin until every task has exited or been killed; returns
 * when the run queue is empty. */
long sched_run(void);

/* True while sched_run() is active (used by the syscall/fault path). */
int sched_active(void);

#endif /* ARM64_SCHED_H */
