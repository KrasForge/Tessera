/* arch/arm64/syscalls.c — SVC syscall dispatch (Issue #13, M2)
 *
 * Strong definitions of arm64_handle_svc() and arm64_user_fault() that
 * override the weak stubs in exceptions.c.  Reached from the EL0 sync vector
 * (issue #12): the full register frame is already saved, so the handler
 * reads the syscall number from x8 and the arguments from x0..x5, and writes
 * the return value back into x0 before the vector restores and ERETs to EL0.
 *
 * Replaces the dispatch table of kernel/syscalls.c.
 */

#include "usermode.h"
#include "exceptions.h"
#include "process.h"
#include "uart_pl011.h"
#include <stdint.h>

/* Weak default so this file links even when process.c is absent (e.g. the
 * standalone EL0 virt harness); process.c provides the strong version. */
__attribute__((weak)) process_t *current_process(void) { return (process_t *)0; }

void arm64_handle_svc(struct trapframe *tf)
{
    uint64_t num = tf->x[8];

    switch (num) {
    case SYS_WRITE: {
        /* (fd, buf, len) — fd ignored; buf is a pointer in the running
         * process address space, which is currently installed, so the
         * kernel can read it directly. */
        const char *buf = (const char *)(uintptr_t)tf->x[1];
        uint64_t    len = tf->x[2];
        for (uint64_t i = 0; i < len; i++)
            uart_putc(buf[i]);
        tf->x[0] = len;
        break;
    }
    case SYS_EXIT:
        kernel_resume((long)tf->x[0]);   /* unwinds to run_user; no return */
        break;                           /* unreachable                    */

    default:
        tf->x[0] = (uint64_t)-1;         /* ENOSYS */
        break;
    }
}

void arm64_user_fault(struct trapframe *tf)
{
    /* The faulting address (FAR_EL1) and PC (ELR_EL1) have already been
     * logged by the exception dumper; add the process identity and access
     * type, mark it killed, and unwind back to process_run() with -1.  The
     * MMU already blocked the offending access, so kernel state is intact. */
    uint32_t   ec = (uint32_t)((tf->esr_el1 >> 26) & 0x3F);
    process_t *p  = current_process();

    uart_puts("  [fault] terminating EL0 process");
    if (p) {
        uart_printf(" pid=%u (%s)", (unsigned)p->pid, p->name);
        p->state = PROC_KILLED;
    }
    if (arm64_ec_class(ec) == EC_CLASS_DATA_ABORT)
        uart_printf(" on %s access",
                    arm64_abort_is_write(tf->esr_el1) ? "write" : "read");
    uart_puts("\r\n");

    kernel_resume(-1);
}
