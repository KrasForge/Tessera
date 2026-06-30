/* tests/arm64/virt/i2s_main.c - I2S API smoke test on the QEMU 'virt' board
 * (Issue #16).
 *
 * QEMU does not emulate the BCM2711 PCM/I2S peripheral, so audible output is
 * verified only on real CM4 hardware.  This harness checks the acceptance
 * criterion that the driver "works in QEMU at the API level: writes to the
 * I2S FIFO do not fault".  It brings up the MMU, maps scratch RAM at the
 * GPIO / PCM / clock-manager register pages, and runs the real i2s_init and
 * the sample-write path against them; if any access faulted, the exception
 * handler would print and halt before the PASS line.
 */

#include "pmm.h"
#include "mmu.h"
#include "vmem.h"
#include "i2s.h"
#include "uart_pl011.h"
#include <stdint.h>

void uart_virt_init(void);

#define GPIO_PAGE  0xFE200000UL
#define PCM_PAGE   0xFE203000UL
#define CM_PAGE    0xFE101000UL
#define PCM_CS_A   (*(volatile uint32_t *)(PCM_PAGE + 0x00))

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt I2S API smoke test (issue #16) ===\r\n");

    pmm_init();
    mmu_init();

    /* Map scratch RAM where the driver expects the peripheral registers, so
     * its MMIO accesses land somewhere valid instead of faulting. */
    vmm_map(phys_alloc_page(), GPIO_PAGE, PAGE_SIZE, VMM_READ | VMM_WRITE);
    vmm_map(phys_alloc_page(), PCM_PAGE,  PAGE_SIZE, VMM_READ | VMM_WRITE);
    vmm_map(phys_alloc_page(), CM_PAGE,   PAGE_SIZE, VMM_READ | VMM_WRITE);
    uart_puts("mapped GPIO/PCM/CM register pages\r\n");

    i2s_init(48000);
    uart_puts("i2s_init(48000) returned\r\n");

    /* Pretend the TX FIFO always has room so the polled writer streams. */
    PCM_CS_A = (1u << 19);          /* CS_A.TXD */

    i2s_play_tone(440, 480);        /* ~10 ms of 440 Hz at 48 kHz */
    uart_puts("wrote 480 sample pairs to the FIFO\r\n");

    uart_puts("I2S-SMOKE: PASS\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
