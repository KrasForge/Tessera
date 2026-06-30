/* arch/arm64/sched.h - cooperative + preemptive RT scheduler
 *                       (Issue #15, M2; preemption and priorities, Issue #20, M4)
 *
 * A scheduler over EL0 processes.  In cooperative mode each task yields with
 * sys_yield; the kernel saves its EL0 register frame, switches the address
 * space (switch_mm), and resumes the next task.  With the M4 timer (issue #19)
 * the same frame-swap machinery is driven preemptively from scheduler_tick():
 * tasks are added at a priority band (REALTIME/NORMAL/IDLE) and a higher-
 * priority runnable task preempts a lower-priority one at the next tick, with
 * round-robin (fair, no starvation) within a band.  This is the ARM port of
 * kernel/scheduler.c; the x86 cli/sti lock path is replaced by DAIF masking.
 */

#ifndef ARM64_SCHED_H
#define ARM64_SCHED_H

#include "process.h"
#include "runqueue.h"
#include <stdint.h>

struct trapframe;

/* Kernel-thread context saved by cpu_switch() (context_switch.S). */
struct cpu_context {
    uint64_t regs[12];   /* x19..x30 */
    uint64_t sp;
};
void cpu_switch(struct cpu_context *prev, struct cpu_context *next);

void sched_init(void);

/* Add an EL0 task at NORMAL priority (cooperative; IRQ masked in EL0): process
 * p starts at `entry` with stack `user_sp`, and `arg0` in its x0.  Returns the
 * task index, or -1 if the queue is full. */
int sched_add(process_t *p, uint64_t entry, uint64_t user_sp, uint64_t arg0);

/* Add an EL0 task at priority band `prio` (SCHED_PRIO_*) with timer quantum
 * `quantum` (0 => default) for preemptive scheduling.  The task runs with IRQs
 * unmasked at EL0 so the timer can preempt it.  Returns the task index or -1. */
int sched_add_prio(process_t *p, uint64_t entry, uint64_t user_sp,
                   uint64_t arg0, int prio, uint32_t quantum);

/* Timer-tick preemption point (issue #20).  Called from the IRQ dispatcher
 * with the interrupted task's register frame; accounts the tick and, if a
 * higher-priority or round-robin successor should run, swaps the frame so the
 * vector ERETs into it.  Weak no-op until the scheduler is running. */
void scheduler_tick(struct trapframe *tf);

/* Test/instrumentation hook: invoked once per tick with the id of the task
 * that will run next (or RQ_NONE when idle).  Weak no-op in the kernel. */
void sched_tick_observer(int task_idx);

/* Run the round-robin until every task has exited or been killed; returns
 * when the run queue is empty. */
long sched_run(void);

/* True while sched_run() is active (used by the syscall/fault path). */
int sched_active(void);

#endif /* ARM64_SCHED_H */
