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

/* Size of the controlled-entry trampoline window; an SVC whose return address
 * falls outside it (for a sandboxed process) is a protocol violation (#35). */
#define SVC_GATE_PAGE 4096ull

/* Weak default so this file links even when process.c is absent (e.g. the
 * standalone EL0 virt harness); process.c provides the strong version. */
__attribute__((weak)) process_t *current_process(void) { return (process_t *)0; }

/* Scheduler hooks (issue #15).  Weak no-op defaults keep the single-process
 * harnesses linkable; arch/arm64/sched.c provides the strong versions. */
__attribute__((weak)) int  sched_active(void) { return 0; }
__attribute__((weak)) void sched_yield(struct trapframe *tf) { (void)tf; }
__attribute__((weak)) void sched_exit(struct trapframe *tf, long code) { (void)tf; (void)code; }
__attribute__((weak)) void sched_kill(struct trapframe *tf) { (void)tf; }

/* Weak syscall trace hook.  Tests override it (e.g. issue #25 counts SVCs to
 * prove the audio data path makes none per block); the kernel default is a
 * no-op. */
__attribute__((weak)) void syscall_trace(uint64_t num) { (void)num; }

/* Audio-graph control plane (issue #28).  Weak defaults so harnesses without
 * the control plane still link; graph_control glue provides the strong ones. */
__attribute__((weak)) long sys_graph_connect(uint32_t s, uint32_t d)    { (void)s; (void)d; return -1; }
__attribute__((weak)) long sys_graph_disconnect(uint32_t s, uint32_t d) { (void)s; (void)d; return -1; }
__attribute__((weak)) long sys_graph_list(void)                         { return -1; }
__attribute__((weak)) long sys_plugin_load(const char *p)               { (void)p; return -1; }
__attribute__((weak)) long sys_plugin_unload(uint32_t pid)              { (void)pid; return -1; }
__attribute__((weak)) long sys_plugin_set_param(uint32_t pid, uint32_t id, uint32_t b)
                                                                        { (void)pid; (void)id; (void)b; return -1; }

void arm64_handle_svc(struct trapframe *tf)
{
    uint64_t num = tf->x[8];
    syscall_trace(num);

    /* Sandbox enforcement (issue #35): a gated process (an untrusted plugin)
     * may only reach the kernel through its controlled entry trampoline.  The
     * SVC's preferred return address (ELR_EL1, the instruction after the SVC)
     * lies in the trampoline page for the sanctioned exit, but in the plugin's
     * own code if the plugin body issued the SVC - which is forbidden, so we
     * kill it instead of servicing the call. */
    process_t *gp = current_process();
    if (gp && gp->svc_gate) {
        uint64_t elr = tf->elr_el1;
        if (elr < gp->svc_gate || elr >= gp->svc_gate + SVC_GATE_PAGE) {
            uart_printf("  [sandbox] illegal SVC #%u from pid=%u (%s)\r\n",
                        (unsigned)num, (unsigned)gp->pid, gp->name);
            gp->state = PROC_KILLED;
            if (sched_active())
                sched_kill(tf);
            else
                kernel_resume(-1);
            return;                 /* unreachable */
        }
    }

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
        if (sched_active())
            sched_exit(tf, (long)tf->x[0]);   /* switch to next task / unwind */
        else
            kernel_resume((long)tf->x[0]);    /* unwinds to run_user          */
        break;                                /* unreachable                  */

    case SYS_YIELD:
        sched_yield(tf);   /* swaps in the next task's frame (no-op if alone) */
        break;

    case SYS_GRAPH_CONNECT:
        tf->x[0] = (uint64_t)sys_graph_connect((uint32_t)tf->x[0], (uint32_t)tf->x[1]);
        break;
    case SYS_GRAPH_DISCONNECT:
        tf->x[0] = (uint64_t)sys_graph_disconnect((uint32_t)tf->x[0], (uint32_t)tf->x[1]);
        break;
    case SYS_GRAPH_LIST:
        tf->x[0] = (uint64_t)sys_graph_list();
        break;

    case SYS_PLUGIN_LOAD:
        tf->x[0] = (uint64_t)sys_plugin_load((const char *)(uintptr_t)tf->x[0]);
        break;
    case SYS_PLUGIN_UNLOAD:
        tf->x[0] = (uint64_t)sys_plugin_unload((uint32_t)tf->x[0]);
        break;
    case SYS_PLUGIN_SET_PARAM:
        tf->x[0] = (uint64_t)sys_plugin_set_param((uint32_t)tf->x[0],
                                                  (uint32_t)tf->x[1],
                                                  (uint32_t)tf->x[2]);
        break;

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

    if (sched_active())
        sched_kill(tf);     /* switch to the next task, or unwind if last */
    else
        kernel_resume(-1);  /* single-process: unwind to run_user         */
}
