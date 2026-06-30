/* tests/arm64/virt/user_main.c — drives the real EL0 entry + SVC machinery
 * (arch/arm64/entry.S + syscalls.c + vectors.S) on the QEMU 'virt' board.
 *
 * Runs with the MMU off (flat addressing), which is enough to exercise the
 * issue #13 acceptance criteria: drop to EL0, take an SVC back to EL1,
 * dispatch sys_write / sys_exit, and trap a privileged instruction.  The
 * address-space isolation that run_user() also performs is covered
 * separately by the #11 host tests.
 *
 * The harness greps the serial log for the greeting and a PASS sentinel.
 */

#include "usermode.h"
#include "uart_pl011.h"
#include <stdint.h>

void uart_virt_init(void);

extern char user_payload[], user_payload_end[];
extern char user_priv_payload[], user_priv_payload_end[];
extern void exceptions_init(void);

static uint64_t ustack1[256] __attribute__((aligned(16)));
static uint64_t ustack2[256] __attribute__((aligned(16)));

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt EL0 + SVC harness (issue #13) ===\r\n");

    exceptions_init();

    /* 1. A normal program: sys_write a greeting, then sys_exit(0). */
    uart_puts("user1 output >> ");
    long c1 = run_user((uint64_t)(uintptr_t)user_payload,
                       (uint64_t)(uintptr_t)(ustack1 + 256), 0, 0);
    uart_printf("user1 sys_exit code=%d\r\n", (int)c1);

    /* 2. A program that executes a privileged instruction -> must trap and
     *    be terminated (run_user returns -1). */
    long c2 = run_user((uint64_t)(uintptr_t)user_priv_payload,
                       (uint64_t)(uintptr_t)(ustack2 + 256), 0, 0);
    uart_printf("user2 privileged-insn code=%d\r\n", (int)c2);

    if (c1 == 0 && c2 == -1)
        uart_puts("VIRT-USER: PASS\r\n");
    else
        uart_puts("VIRT-USER: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
