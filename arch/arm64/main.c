/* arch/arm64/main.c — Tessera AArch64 C entry point (Issues #2, #3, #4)
 *
 * Called from boot/start.S after:
 *   - EL2 → EL1 transition (or direct EL1 entry under QEMU)
 *   - SP_EL1 initialised from __stack_top
 *   - BSS section zeroed
 *
 * Brings up:
 *   - GPIO subsystem and CM4 on-board LED (GPIO 42, active-high) [Issue #4]
 *   - PL011 UART at 115200 8N1, polled [Issue #3]
 *   - Boot banner "Tessera ARM boot OK\r\n" (required by CI smoke test) [Issue #5]
 *   - 1 Hz LED heartbeat loop to demonstrate system-counter and GPIO [Issue #4]
 */

#include "gpio.h"
#include "uart_pl011.h"
#include <stdint.h>

/* CM4 / Pi 4 on-board LED.  GPIO 42, active-high (BCM2711 datasheet §5). */
#define CM4_LED_PIN 42u

/* Busy-wait using the ARM generic system counter (CNTPCT_EL0). */
static void delay_ms(uint64_t ms)
{
    uint64_t freq, start, now;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
    uint64_t target = start + (freq / 1000ULL) * ms;
    do {
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    } while ((now - start) < (target - start));
}

void kmain(void)
{
    /* GPIO first: turning the LED on immediately gives a visual "alive"
     * signal that is independent of UART (useful on real hardware). */
    gpio_init();
    gpio_set_function(CM4_LED_PIN, GPIO_FUNC_OUTPUT);
    gpio_set(CM4_LED_PIN);

    /* Initialise the PL011 UART and emit the boot banner.
     * The string "Tessera ARM boot OK" is grepped by the CI workflow. */
    uart_init();
    uart_puts("Tessera ARM boot OK\r\n");

    /* Report the running exception level (should be EL1). */
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    el >>= 2;
    uart_printf("Running at EL%u\r\n", (unsigned)el);

    /* 1 Hz LED heartbeat: 500 ms on, 500 ms off. */
    while (1) {
        gpio_set(CM4_LED_PIN);
        delay_ms(500);
        gpio_clear(CM4_LED_PIN);
        delay_ms(500);
    }
}
