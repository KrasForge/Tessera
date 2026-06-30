/* tests/arm64/virt/sine_main.c - sine-generator API smoke test on QEMU 'virt'
 * (Issue #18).
 *
 * Audible output is a real-CM4 check; here we confirm the high-level control
 * path (audio_sine_start, a runtime frequency change, service, stop) drives
 * the DMA streaming backend against scratch-mapped peripheral registers with
 * the MMU on and never faults.
 */

#include "pmm.h"
#include "mmu.h"
#include "vmem.h"
#include "sine_gen.h"
#include "uart_pl011.h"
#include <stdint.h>

void uart_virt_init(void);

#define DMA_PAGE   0xFE007000UL
#define CM_PAGE    0xFE101000UL
#define GPIO_PAGE  0xFE200000UL
#define PCM_PAGE   0xFE203000UL

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt sine-generator smoke test (issue #18) ===\r\n");

    pmm_init();
    mmu_init();
    vmm_map(phys_alloc_page(), DMA_PAGE,  PAGE_SIZE, VMM_READ | VMM_WRITE);
    vmm_map(phys_alloc_page(), CM_PAGE,   PAGE_SIZE, VMM_READ | VMM_WRITE);
    vmm_map(phys_alloc_page(), GPIO_PAGE, PAGE_SIZE, VMM_READ | VMM_WRITE);
    vmm_map(phys_alloc_page(), PCM_PAGE,  PAGE_SIZE, VMM_READ | VMM_WRITE);

    audio_sine_start(440);
    uart_puts("audio_sine_start(440)\r\n");
    for (int i = 0; i < 4; i++)
        audio_sine_service();

    audio_sine_set_freq(880);            /* phase-continuous runtime change */
    audio_sine_set_amplitude(16384);
    uart_puts("audio_sine_set_freq(880) + amplitude\r\n");
    for (int i = 0; i < 4; i++)
        audio_sine_service();

    audio_sine_stop();
    uart_puts("audio_sine_stop\r\n");

    uart_puts("SINE-GEN: PASS\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
