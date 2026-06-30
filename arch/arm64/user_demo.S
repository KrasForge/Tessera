/* arch/arm64/user_demo.S — minimal EL0 payloads for the issue #13 smoke test.
 *
 * Position-independent (uses adr) so the bytes can be copied into a process
 * page and mapped at any user VA.  Two payloads:
 *   user_payload       — sys_write a greeting, then sys_exit(0)
 *   user_priv_payload  — execute a privileged instruction (must trap to EL1)
 */

    .section .text

    .global user_payload
    .global user_payload_end
user_payload:
    mov     x8, #1                  /* SYS_WRITE        */
    mov     x0, #1                  /* fd = 1 (stdout)  */
    adr     x1, .Lmsg               /* buf (PC-relative) */
    mov     x2, #(.Lmsg_end - .Lmsg)
    svc     #0
    mov     x8, #2                  /* SYS_EXIT         */
    mov     x0, #0                  /* code = 0         */
    svc     #0
0:  b       0b                      /* unreachable      */
.Lmsg:
    .ascii  "Hello from EL0 via SVC!\r\n"
.Lmsg_end:
    .balign 4               /* keep following instructions 4-byte aligned */
user_payload_end:

    .global user_priv_payload
    .global user_priv_payload_end
user_priv_payload:
    mrs     x0, sctlr_el1           /* privileged read -> traps from EL0 */
0:  b       0b
user_priv_payload_end:

/* Writes to the kernel address passed in x0 (run_user arg0).  The MMU maps
 * kernel memory as EL1-only, so this faults and the process is killed. */
    .global user_badwrite_payload
    .global user_badwrite_payload_end
user_badwrite_payload:
    mov     x9, #0xBAD
    str     x9, [x0]                /* write to kernel memory -> data abort */
0:  b       0b
user_badwrite_payload_end:

/* Round-robin task (issue #15).  x0 = pointer to this task's private 1-byte
 * data page.  Reads its byte, writes it to UART and yields a few times, then
 * exits with that byte as the code.  Because the byte is read from the
 * task's own address space, a correct exit code proves there was no TLB
 * aliasing across context switches. */
    .global user_task
    .global user_task_end
user_task:
    mov     x20, x0                 /* private data pointer (callee-saved) */
    ldrb    w21, [x20]              /* this task's byte                    */
    mov     x19, #5                 /* iterations                          */
1:  mov     x8, #1                  /* SYS_WRITE                           */
    mov     x0, #1
    mov     x1, x20
    mov     x2, #1
    svc     #0
    mov     x8, #3                  /* SYS_YIELD                           */
    svc     #0
    subs    x19, x19, #1
    b.ne    1b
    mov     x8, #2                  /* SYS_EXIT(byte)                      */
    mov     x0, x21
    svc     #0
2:  b       2b
user_task_end:

/* Preemption task (issue #20).  A CPU-bound busy loop that never makes a
 * syscall, so it can only leave the CPU by being *preempted* by the timer
 * IRQ.  x0 = iteration count.  It spins that many times, then sys_exit(0).
 * Two such NORMAL tasks interleaving, or a REALTIME one running to completion
 * ahead of them, demonstrates real priority-based preemption. */
    .global user_busy
    .global user_busy_end
user_busy:
    mov     x19, x0                 /* iteration counter (callee-saved)    */
1:  subs    x19, x19, #1
    b.ne    1b                      /* tight loop: no SVC, no yield        */
    mov     x8, #2                  /* SYS_EXIT                            */
    mov     x0, #0
    svc     #0
2:  b       2b
user_busy_end:

/* NEON task (issue #15).  The first FP instruction traps (FPEN), the kernel
 * lazily enables FP and restores this task's NEON state, then 2.0+3.0 is
 * computed.  Exits 0 on the correct result, 1 otherwise. */
    .global user_fpu_task
    .global user_fpu_task_end
user_fpu_task:
    fmov    d0, #2.0                /* first FP use -> EC 0x07 trap        */
    fmov    d1, #3.0
    fadd    d0, d0, d1              /* 5.0                                 */
    fmov    d1, #5.0
    fcmp    d0, d1
    mov     x0, #1                  /* default: failure code               */
    b.ne    3f
    mov     x0, #0                  /* success                             */
3:  mov     x8, #2                  /* SYS_EXIT                            */
    svc     #0
4:  b       4b
user_fpu_task_end:
