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
