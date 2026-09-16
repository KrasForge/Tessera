/* arch/arm64/irq.c - top-level IRQ dispatch (Issue #19, M4)
 *
 * Called from the IRQ exception vector (vectors.S via arm64_exception).
 * Acknowledges the interrupt at the GIC, routes it by INTID, and signals
 * end-of-interrupt.  Replaces the x86 dispatch in kernel/interrupt_handlers.c
 * with ARM IRQ numbers.
 */

#include "exceptions.h"
#include "gic.h"
#include "timer.h"
#include "budget.h"
#include <stdint.h>

/* DMA completion interrupt from the I2S audio channel (issue #17).  The
 * BCM2711 routes DMA channel IRQs to SPIs starting at 0x20 (channel 0 = 32);
 * channel 5 used by the audio driver is INTID 37.  Weak handler so this file
 * links without the audio subsystem. */
#define DMA_AUDIO_IRQ 37u
__attribute__((weak)) void audio_dma_irq(void) { }

/* Scheduler preemption point (arch/arm64/sched.c).  Weak no-op default so
 * harnesses that bring up the timer without the scheduler (e.g. the standalone
 * GIC+timer test) still link. */
__attribute__((weak)) void scheduler_tick(struct trapframe *tf) { (void)tf; }

/* Independent banked virtual-budget PPI. The physical cadence timer always
 * reaches timer_tick; a plugin budget never replaces its compare register.
 * A budget IRQ at EL0 EOIs and returns through that core's saved context. */
__attribute__((weak)) int budget_timer_irq(struct trapframe *tf, uint32_t iar)
{
    (void)tf; (void)iar;
    return 0;
}

void arm64_irq(struct trapframe *tf)
{
    uint32_t iar = gic_ack();
    uint32_t id  = iar & 0x3FF;

    if (id >= GIC_SPURIOUS)
        return;                 /* spurious: no handler, no EOI */

    if (id == BUDGET_TIMER_IRQ) {
        budget_timer_irq(tf, iar);
    } else if (id == TIMER_IRQ) {
        timer_tick();
        scheduler_tick(tf);
    } else if (id == DMA_AUDIO_IRQ) {
        audio_dma_irq();
    }
    /* other INTIDs: acknowledged and EOI'd below (no-op) */

    gic_eoi(iar);
}
