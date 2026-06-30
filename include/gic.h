/* include/gic.h - ARM GIC-400 (GICv2) interrupt controller (Issue #19, M4)
 *
 * The BCM2711 uses a GIC-400 (a GICv2): a distributor plus a per-CPU
 * interface.  This replaces the x86 APIC.  The base addresses default to the
 * BCM2711 map and can be overridden at build time (the QEMU 'virt' board puts
 * the GIC at 0x08000000 / 0x08010000) so the same driver runs in the timer
 * test harness.
 */

#ifndef GIC_H
#define GIC_H

#include <stdint.h>

#ifndef GIC_DIST_BASE
#define GIC_DIST_BASE 0xFF841000UL   /* BCM2711 GICD */
#endif
#ifndef GIC_CPU_BASE
#define GIC_CPU_BASE  0xFF842000UL   /* BCM2711 GICC */
#endif

/* A read of the acknowledge register returns this when no interrupt is
 * pending (spurious); it must not be EOI'd. */
#define GIC_SPURIOUS 1023u

/* Enable the distributor and this CPU's interface (priority mask open). */
void gic_init(void);

/* Enable a specific interrupt ID, route it to CPU 0 at highest priority. */
void gic_enable_irq(uint32_t irq);

/* Acknowledge the pending interrupt: returns the full IAR value (INTID in
 * bits [9:0]). */
uint32_t gic_ack(void);

/* Signal end-of-interrupt for the value previously returned by gic_ack(). */
void gic_eoi(uint32_t iar);

#endif /* GIC_H */
