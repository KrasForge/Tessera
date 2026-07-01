/* arch/arm64/plugin_trampoline.S - controlled EL0 entry for a plugin
 *                                   (Issue #24, M5; generalised in #36)
 *
 * The host maps a copy of this code into the plugin's address space and enters
 * it at EL0 with x0 = address of a parameter block.  The block names the plugin
 * function to call and its up to five AAPCS64 arguments:
 *
 *     [x0 + 0]   uint64  function VA (resolved from the plugin's symtab)
 *     [x0 + 8]   uint64  arg0 -> x0
 *     [x0 + 16]  uint64  arg1 -> x1
 *     [x0 + 24]  uint64  arg2 -> x2
 *     [x0 + 32]  uint64  arg3 -> x3
 *     [x0 + 40]  uint64  arg4 -> x4
 *
 * This drives plugin_abi_version(), plugin_init(sample_rate, block_size), and
 * plugin_process_block(in_l, in_r, out_l, out_r, n_frames) alike.  After the
 * call it performs SYS_EXIT with the function's return value, handing control
 * back to the host through the kernel's syscall path.  It is position-
 * independent (no absolute addresses), so it runs at whatever VA the host maps
 * it to, and it is the sole sanctioned SVC site for a sandboxed plugin (#35).
 */

    .section .text
    .global plugin_tramp_start
    .global plugin_tramp_end
plugin_tramp_start:
    mov     x19, x0                 /* param block pointer            */
    ldr     x20, [x19, #0]          /* function VA                    */
    ldr     x0,  [x19, #8]          /* arg0                           */
    ldr     x1,  [x19, #16]         /* arg1                           */
    ldr     x2,  [x19, #24]         /* arg2                           */
    ldr     x3,  [x19, #32]         /* arg3                           */
    ldr     x4,  [x19, #40]         /* arg4                           */
    blr     x20                     /* call the plugin function       */
    mov     x8, #2                  /* SYS_EXIT                       */
    svc     #0                      /* exit with x0 = return value    */
0:  b       0b                      /* unreachable                    */
plugin_tramp_end:
