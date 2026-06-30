/* tests/arm64/virt/fault_main.c — full-stack fault-containment harness on the
 * QEMU 'virt' board (Issue #14).
 *
 * Unlike the EL0 harness (which runs MMU-off), this brings up the REAL
 * pmm + mmu + process + exception stack with the MMU ENABLED, so the
 * hardware actually enforces the kernel/user split.  It then:
 *   1. runs a process that writes to a kernel variable -> trapped + killed,
 *   2. confirms the kernel variable is intact (the write never landed),
 *   3. runs a second process that prints normally (kernel kept running).
 *
 * The real arch/arm64 sources are compiled with the virt memory map
 * (-DPHYS_RAM_START=0x40000000 etc.) so pmm/mmu target virt RAM/MMIO.
 *
 * The harness greps the serial log for the greeting and a PASS sentinel.
 */

#include "pmm.h"
#include "mmu.h"
#include "vmem.h"
#include "process.h"
#include "exceptions.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void  uart_virt_init(void);
void *memcpy(void *, const void *, size_t);

extern char user_payload[], user_payload_end[];
extern char user_badwrite_payload[], user_badwrite_payload_end[];

/* A kernel variable the misbehaving process will try (and fail) to clobber. */
static volatile uint64_t g_kernel_canary = 0xCAFEF00DDEADBEEFUL;

#define USER_ENTRY  USER_VA_BASE
#define USER_SP     (USER_VA_BASE + 0x10000 + PAGE_SIZE)

static process_t *load(const char *name, char *code, size_t len)
{
    process_t *p = process_create(name);
    uintptr_t code_pa = phys_alloc_page();
    memcpy((void *)code_pa, code, len);
    process_map(p, code_pa, USER_VA_BASE, PAGE_SIZE, VMM_READ | VMM_EXEC);
    uintptr_t stack_pa = phys_alloc_page();
    process_map(p, stack_pa, USER_VA_BASE + 0x10000, PAGE_SIZE,
                VMM_READ | VMM_WRITE);
    return p;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt fault-containment harness (issue #14) ===\r\n");

    pmm_init();
    mmu_init();             /* MMU on: hardware enforces the kernel/user split */
    exceptions_init();
    uart_puts("pmm+mmu+vectors up (MMU enabled)\r\n");

    uint64_t before = g_kernel_canary;

    /* 1. Misbehaving process: write to the kernel canary's address. */
    process_t *bad = load("bad-plugin", user_badwrite_payload,
                          (size_t)(user_badwrite_payload_end - user_badwrite_payload));
    long bc = process_run(bad, USER_ENTRY, USER_SP,
                          (uint64_t)(uintptr_t)&g_kernel_canary);
    uart_printf("bad-plugin exit=%d state=%s\r\n",
                (int)bc, bad->state == PROC_KILLED ? "KILLED" : "?");
    process_destroy(bad);

    /* 2. Kernel data intact? */
    int intact = (g_kernel_canary == before);
    uart_printf("kernel canary intact: %s\r\n", intact ? "yes" : "NO");

    /* 3. Second process runs normally. */
    uart_puts("good-plugin output >> ");
    process_t *good = load("good-plugin", user_payload,
                           (size_t)(user_payload_end - user_payload));
    long gc = process_run(good, USER_ENTRY, USER_SP, 0);
    uart_printf("good-plugin exit=%d\r\n", (int)gc);
    process_destroy(good);

    if (bc == -1 && intact && gc == 0)
        uart_puts("VIRT-FAULT: PASS\r\n");
    else
        uart_puts("VIRT-FAULT: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
