/* arch/arm64/usermode.h — EL0 entry + SVC syscall ABI (Issue #13, M2)
 *
 * The ARM replacement for x86 ring-3 entry (kernel/user_mode.asm) and the
 * int 0x80 / syscall dispatch (kernel/syscalls.c).
 *
 * Syscall ABI (AArch64, Linux-like): x8 = syscall number, x0..x5 = args,
 * return value in x0, instruction `svc #0`.
 */

#ifndef ARM64_USERMODE_H
#define ARM64_USERMODE_H

#include <stdint.h>

struct trapframe;

/* Minimal syscall numbers for the M2 smoke test. */
#define SYS_WRITE  1   /* (fd, buf, len) -> bytes written; writes to UART */
#define SYS_EXIT   2   /* (code) -> does not return; unwinds to run_user  */

/* Drop to EL0 and run a process until it exits (or faults).
 *
 *   entry    - EL0 entry point (virtual address in the process space)
 *   user_sp  - initial SP_EL0
 *   ttbr0    - TTBR0_EL1 value to install (process root | ASID); ignored
 *              when the MMU is off
 *
 * Returns the value passed to sys_exit, or -1 if the process was killed by a
 * fault (see arm64_user_fault).  Implemented in entry.S: it saves the kernel
 * callee-saved context, ERETs to EL0, and "returns" only when sys_exit or a
 * fault calls kernel_resume(). */
long run_user(uint64_t entry, uint64_t user_sp, uint64_t ttbr0);

/* Resume the kernel context saved by run_user(), making run_user() return
 * `code`.  Called from the SVC/fault path; never returns to its caller. */
void kernel_resume(long code);

/* SVC dispatcher (strong definition overrides the weak stub in exceptions.c). */
void arm64_handle_svc(struct trapframe *tf);

/* Called when an EL0 process takes a fault; terminates it via kernel_resume. */
void arm64_user_fault(struct trapframe *tf);

#endif /* ARM64_USERMODE_H */
