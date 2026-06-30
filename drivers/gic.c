/* drivers/gic.c - ARM GIC-400 (GICv2) interrupt controller (Issue #19, M4)
 *
 * Minimal GICv2 bring-up: enable the distributor and CPU interface, and
 * enable/ack/EOI individual interrupts.  Group 0, priority mask wide open.
 */

#include "gic.h"
#include <stdint.h>

/* Distributor registers */
#define GICD_CTLR        (*(volatile uint32_t *)(GIC_DIST_BASE + 0x000))
#define GICD_ISENABLER(n)(*(volatile uint32_t *)(GIC_DIST_BASE + 0x100 + 4u * (n)))
#define GICD_IPRIORITY   ((volatile uint8_t  *)(GIC_DIST_BASE + 0x400))
#define GICD_ITARGETS    ((volatile uint8_t  *)(GIC_DIST_BASE + 0x800))

/* CPU interface registers */
#define GICC_CTLR        (*(volatile uint32_t *)(GIC_CPU_BASE + 0x000))
#define GICC_PMR         (*(volatile uint32_t *)(GIC_CPU_BASE + 0x004))
#define GICC_IAR         (*(volatile uint32_t *)(GIC_CPU_BASE + 0x00C))
#define GICC_EOIR        (*(volatile uint32_t *)(GIC_CPU_BASE + 0x010))

void gic_init(void)
{
    GICD_CTLR = 1;        /* enable the distributor          */
    GICC_PMR  = 0xF0;     /* accept all interrupt priorities */
    GICC_CTLR = 1;        /* enable this CPU's interface      */
}

void gic_enable_irq(uint32_t irq)
{
    GICD_ISENABLER(irq / 32) = 1u << (irq % 32);
    GICD_IPRIORITY[irq] = 0;          /* highest priority */
    if (irq >= 32)                    /* SPIs target CPU 0; PPIs are local */
        GICD_ITARGETS[irq] = 0x01;
}

uint32_t gic_ack(void)
{
    return GICC_IAR;
}

void gic_eoi(uint32_t iar)
{
    GICC_EOIR = iar;
}
