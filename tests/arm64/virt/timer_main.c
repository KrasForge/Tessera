/* tests/arm64/virt/timer_main.c - GIC + generic-timer test on QEMU 'virt'
 * (Issue #19).
 *
 * Unlike the audio peripherals, QEMU emulates the GICv2 and the ARM generic
 * timer accurately, so this is a real interrupt test: install the vectors,
 * bring up the GIC, program the EL1 physical timer for 1 kHz, enable IRQs,
 * and count how many timer interrupts fire over one second of CNTPCT.  A
 * count near 1000 with no lockup proves the GIC ack/EOI path and the timer
 * IRQ dispatch work and produce no interrupt storm or starvation.
 *
 * Built with the virt GIC base addresses (0x08000000 / 0x08010000).
 */

#include "gic.h"
#include "timer.h"
#include "uart_pl011.h"
#include <stdint.h>

void uart_virt_init(void);
void exceptions_init(void);

static uint64_t rd_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt GIC + timer test (issue #19) ===\r\n");

    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uart_printf("EL%u, CNTFRQ=%u Hz\r\n", (unsigned)(el >> 2), (unsigned)freq);

    exceptions_init();              /* VBAR_EL1 -> vectors        */
    gic_init();                     /* distributor + CPU iface    */
    timer_init(1000);               /* 1 kHz EL1 physical timer   */
    __asm__ volatile("msr daifclr, #2");   /* unmask IRQ          */
    uart_puts("GIC + 1 kHz timer running, IRQs enabled\r\n");

    /* Count ticks over one second of real time. */
    uint64_t start = rd_cntpct();
    while (rd_cntpct() - start < freq)
        __asm__ volatile("nop");
    uint64_t ticks = timer_ticks();

    timer_stop();
    __asm__ volatile("msr daifset, #2");

    uart_printf("timer ticks in 1 s = %u (expect ~1000)\r\n", (unsigned)ticks);

    int ok = (ticks >= 995 && ticks <= 1005);   /* drift-free: within 0.5% */
    uart_puts(ok ? "TIMER: PASS\r\n" : "TIMER: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
