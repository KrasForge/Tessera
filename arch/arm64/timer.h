/* arch/arm64/timer.h - ARM generic timer (Issue #19, M4)
 *
 * The EL1 physical timer (CNTP_TVAL_EL0 / CNTP_CTL_EL0) replaces the APIC
 * one-shot timer.  It raises PPI INTID 30 through the GIC at a programmable
 * tick rate, the heartbeat for the real-time scheduler.
 */

#ifndef ARM64_TIMER_H
#define ARM64_TIMER_H

#include <stdint.h>

/* EL1 physical timer PPI. */
#define TIMER_IRQ 30u

/* Program the timer for `hz` ticks/second, enable it, and unmask its GIC
 * interrupt.  gic_init() must have run. */
void timer_init(uint32_t hz);

/* Called from the IRQ dispatcher when TIMER_IRQ fires: reload for the next
 * interval and bump the tick count.  The scheduler preemption point
 * (scheduler_tick, see sched.h) is invoked separately by the dispatcher with
 * the interrupted task's frame. */
void timer_tick(void);

/* Total ticks since timer_init(). */
uint64_t timer_ticks(void);

/* Absolute CNTPCT value of the next scheduled tick (CNTP_CVAL).  After a tick
 * has fired and been serviced, (timer_deadline() - timer_interval()) is the
 * deadline that just elapsed - used to measure IRQ-to-thread wakeup latency. */
uint64_t timer_deadline(void);

/* Counter ticks per timer interval (CNTFRQ / hz). */
uint64_t timer_interval(void);

/* Disable the timer and mask its interrupt. */
void timer_stop(void);

#endif /* ARM64_TIMER_H */
