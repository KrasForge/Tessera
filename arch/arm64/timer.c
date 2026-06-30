/* arch/arm64/timer.c - ARM generic timer (Issue #19, M4) */

#include "timer.h"
#include "gic.h"
#include <stdint.h>

#define CNTP_CTL_ENABLE  (1u << 0)
#define CNTP_CTL_IMASK   (1u << 1)

static uint64_t          g_interval;   /* counter ticks per scheduler tick */
static uint64_t          g_deadline;   /* absolute CNTP_CVAL of next tick   */
static volatile uint64_t g_ticks;

/* Weak default; the preemptive scheduler (issue #20) overrides this. */
__attribute__((weak)) void scheduler_tick(void) { }

void timer_init(uint32_t hz)
{
    uint64_t freq, now;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    g_interval = freq / (hz ? hz : 1u);
    g_deadline = now + g_interval;
    g_ticks = 0;

    /* Absolute compare deadline (CVAL) rather than a relative reload (TVAL),
     * so handler latency does not accumulate into clock drift. */
    __asm__ volatile("msr cntp_cval_el0, %0" :: "r"(g_deadline));
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)CNTP_CTL_ENABLE));

    gic_enable_irq(TIMER_IRQ);
}

void timer_tick(void)
{
    /* Advance the deadline by a fixed interval: jitter-free 1 kHz. */
    g_deadline += g_interval;
    __asm__ volatile("msr cntp_cval_el0, %0" :: "r"(g_deadline));
    g_ticks++;
    scheduler_tick();
}

uint64_t timer_ticks(void)
{
    return g_ticks;
}

void timer_stop(void)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)CNTP_CTL_IMASK));
}
