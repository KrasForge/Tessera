/* tests/arm64/virt/sched_main.c - full-stack context-switch harness on the
 * QEMU 'virt' board (Issue #15).
 *
 * Brings up the real pmm + mmu + process + exception + scheduler stack with
 * the MMU enabled, creates three EL0 tasks, and round-robins them:
 *   - task A and task B each read a private 1-byte data page and print it,
 *     yielding between iterations.  Their exit codes ('A' / 'B') come from
 *     their own address spaces, so correct codes prove there is no TLB
 *     aliasing across the context switches.
 *   - an FP task computes 2.0 + 3.0 using NEON; its first FP instruction
 *     traps and the kernel lazily enables/restores FP.  Exit code 0 means the
 *     NEON result was correct.
 *
 * The harness greps the serial log for the interleaved A/B output and a PASS
 * sentinel.
 */

#include "pmm.h"
#include "mmu.h"
#include "vmem.h"
#include "process.h"
#include "sched.h"
#include "exceptions.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void  uart_virt_init(void);
void *memcpy(void *, const void *, size_t);
void  fpu_disable(void);

extern char user_task[], user_task_end[];
extern char user_fpu_task[], user_fpu_task_end[];

#define USER_CODE_VA   USER_VA_BASE
#define USER_STACK_VA  (USER_VA_BASE + 0x10000)
#define USER_DATA_VA   (USER_VA_BASE + 0x20000)
#define USER_SP        (USER_STACK_VA + PAGE_SIZE)

/* Create a process, map its code (RX) and stack (RW). */
static process_t *make_proc(const char *name, char *code, size_t len)
{
    process_t *p = process_create(name);
    uintptr_t code_pa = phys_alloc_page();
    memcpy((void *)code_pa, code, len);
    process_map(p, code_pa, USER_CODE_VA, PAGE_SIZE, VMM_READ | VMM_EXEC);
    uintptr_t stack_pa = phys_alloc_page();
    process_map(p, stack_pa, USER_STACK_VA, PAGE_SIZE, VMM_READ | VMM_WRITE);
    return p;
}

/* Add a private data page holding `byte` at USER_DATA_VA. */
static void add_data(process_t *p, char byte)
{
    uintptr_t data_pa = phys_alloc_page();
    *(volatile char *)data_pa = byte;
    process_map(p, data_pa, USER_DATA_VA, PAGE_SIZE, VMM_READ | VMM_WRITE);
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt context-switch harness (issue #15) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();
    fpu_disable();          /* FP trapped until first use (lazy FPU) */
    sched_init();

    size_t tlen = (size_t)(user_task_end - user_task);
    size_t flen = (size_t)(user_fpu_task_end - user_fpu_task);

    process_t *a = make_proc("task-A", user_task, tlen);
    add_data(a, 'A');
    process_t *b = make_proc("task-B", user_task, tlen);
    add_data(b, 'B');
    process_t *f = make_proc("task-FPU", user_fpu_task, flen);

    sched_add(a, USER_CODE_VA, USER_SP, USER_DATA_VA);
    sched_add(b, USER_CODE_VA, USER_SP, USER_DATA_VA);
    sched_add(f, USER_CODE_VA, USER_SP, 0);

    uart_puts("round-robin output >> ");
    sched_run();
    uart_puts(" <<\r\n");

    uart_printf("task-A exit=0x%x (expect 0x41)\r\n", (unsigned)a->exit_code);
    uart_printf("task-B exit=0x%x (expect 0x42)\r\n", (unsigned)b->exit_code);
    uart_printf("task-FPU exit=%d (expect 0)\r\n", (int)f->exit_code);

    int ok = (a->exit_code == 'A') && (a->state == PROC_ZOMBIE) &&
             (b->exit_code == 'B') && (b->state == PROC_ZOMBIE) &&
             (f->exit_code == 0)   && (f->state == PROC_ZOMBIE);

    uart_puts(ok ? "VIRT-SCHED: PASS\r\n" : "VIRT-SCHED: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
