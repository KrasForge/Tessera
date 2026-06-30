/* tests/arm64/virt/test_main.c — drives the real exception machinery
 * (arch/arm64/vectors.S + exceptions.c) on the QEMU 'virt' board:
 * install VBAR_EL1, trap a recoverable undefined instruction, and report.
 * The harness greps the serial log for "caught+recovered". */

#include "exceptions.h"
#include "uart_pl011.h"   /* uart_* prototypes (definitions from uart_virt.c) */
#include <stdint.h>

void uart_virt_init(void);

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt exception harness (issue #12) ===\r\n");

    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    uart_printf("running at EL%u\r\n", (unsigned)(el >> 2));

    exceptions_init();
    uart_puts("VBAR_EL1 installed\r\n");

    exceptions_selftest();     /* prints "...caught+recovered" on success */

    uart_puts("harness done\r\n");
    for (;;)
        __asm__ volatile("wfe");
}
