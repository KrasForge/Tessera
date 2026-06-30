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
#include "uart_pl011.h"
#include <stdint.h>

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
    (void)tf;
    kernel_resume(-1);                   /* terminate the faulting process */
}
