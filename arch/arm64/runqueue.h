/* arch/arm64/runqueue.h - priority run queue for the RT scheduler (Issue #20)
 *
 * This is the pure scheduling *policy* of Tessera's real-time scheduler, the
 * ARM port of the decision logic in kernel/scheduler.c.  It is deliberately
 * free of any ARM register or MMU access so the same code that runs in the
 * kernel is exercised by the host unit tests (tests/arm64/rtsched_test.c).
 *
 * Tasks live in three priority bands (lower number = higher priority):
 *
 *   SCHED_PRIO_REALTIME  audio callback / hard-RT threads
 *   SCHED_PRIO_NORMAL    host process, plugin control paths
 *   SCHED_PRIO_IDLE      runs only when nothing else is runnable
 *
 * A higher-priority runnable task always preempts a lower-priority one.
 * Within a band, tasks are scheduled round-robin (FIFO) with a fixed timer
 * quantum, which guarantees fairness with no starvation.
 *
 * Per-CPU isolation (a separate run queue per core, with the audio core
 * isolated) is the subject of issue #21; this module implements one run queue
 * and keeps its state file-local so it can be instantiated per CPU later.
 */

#ifndef ARM64_RUNQUEUE_H
#define ARM64_RUNQUEUE_H

#include <stdint.h>

/* Priority bands (lower value = higher priority). */
#define SCHED_PRIO_REALTIME 0
#define SCHED_PRIO_NORMAL   1
#define SCHED_PRIO_IDLE     2
#define SCHED_NPRIO         3

#define RQ_MAX_TASKS        128
#define RQ_QUANTUM_DEFAULT  5     /* timer ticks per round-robin slice */
#define RQ_NONE             (-1)

/* Reset the run queue to empty. */
void rq_init(void);

/* Register task `id` (0..RQ_MAX_TASKS-1) as runnable at priority `prio`, with
 * round-robin quantum `quantum` (0 selects RQ_QUANTUM_DEFAULT).  Returns 0 on
 * success, -1 on a bad id or priority. */
int rq_add(int id, int prio, uint32_t quantum);

/* State transitions. */
void rq_block(int id);     /* blocked: not eligible until rq_unblock()     */
void rq_unblock(int id);   /* back to ready, requeued at the tail of band   */
void rq_remove(int id);    /* terminated: removed for good                  */

/* Priority inheritance / boost: set the *effective* priority used for
 * scheduling without losing the task's base priority (rq_base_prio). */
void rq_set_effective_prio(int id, int prio);
int  rq_base_prio(int id);
int  rq_effective_prio(int id);

/* Highest-priority runnable task, round-robin within its band; RQ_NONE if no
 * task is runnable. */
int rq_pick(void);

/* The task currently marked running (set by rq_commit). */
int rq_current(void);

/* Make `id` the running task: reset its slice and rotate it to the tail of
 * its band so equal-priority peers run before it again (fair round-robin). */
void rq_commit(int id);

/* Account one timer tick to the running task and return the id that should
 * run next.  The result equals rq_current() when the current task keeps the
 * CPU; otherwise it is the preempting (higher-priority) or round-robin
 * successor, which the caller installs and then rq_commit()s.  RQ_NONE means
 * nothing is runnable (go idle). */
int rq_tick(void);

/* Total timer ticks the task has been the running task (fairness stats). */
uint64_t rq_ran_ticks(int id);

/* ---- priority-inheriting mutex (IPC) stub ----------------------------------
 *
 * A minimal mutex that prevents priority inversion: if a high-priority task
 * blocks on a mutex held by a lower-priority task, the holder inherits the
 * waiter's priority until it releases, so it cannot be kept off the CPU by an
 * unrelated mid-priority task.  This is the stub required by issue #20; full
 * blocking IPC is a later milestone.
 */
typedef struct {
    int holder;   /* task id holding the mutex, or RQ_NONE        */
    int waiter;   /* highest-priority task blocked on it, or NONE */
} rq_mutex_t;

void rq_mutex_init(rq_mutex_t *m);

/* Try to acquire `m` for task `id`.  Returns 1 if acquired immediately.
 * Returns 0 if the mutex is held: `id` is blocked and, if it outranks the
 * holder, the holder inherits `id`'s priority (priority inheritance). */
int rq_mutex_lock(rq_mutex_t *m, int id);

/* Release `m` held by `id`: restore `id`'s base priority and, if a waiter is
 * queued, hand the mutex to it and make it runnable again. */
void rq_mutex_unlock(rq_mutex_t *m, int id);

#endif /* ARM64_RUNQUEUE_H */
