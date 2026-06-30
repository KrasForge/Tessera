/* arch/arm64/plugin_trampoline.S - controlled EL0 entry for a plugin
 *                                   (Issue #24, M5)
 *
 * The host maps a copy of this code into the plugin's address space and enters
 * it at EL0 with x0 = address of a parameter block:
 *
 *     [x0 + 0]  uint64  plugin_init VA (resolved from the plugin's symtab)
 *     [x0 + 8]  uint32  sample_rate
 *     [x0 + 12] uint32  block_size
 *
 * It calls plugin_init(sample_rate, block_size) and then performs SYS_EXIT
 * with plugin_init's return value, handing control back to the host via the
 * kernel's syscall path.  It is position-independent (no absolute addresses),
 * so it runs correctly at whatever VA the host maps it to.
 */

    .section .text
    .global plugin_tramp_start
    .global plugin_tramp_end
plugin_tramp_start:
    mov     x19, x0                 /* param block pointer            */
    ldr     x20, [x19, #0]          /* plugin_init VA                 */
    ldr     w0,  [x19, #8]          /* sample_rate -> arg0            */
    ldr     w1,  [x19, #12]         /* block_size  -> arg1            */
    blr     x20                     /* plugin_init(rate, block)       */
    mov     x8, #2                  /* SYS_EXIT                       */
    svc     #0                      /* exit with x0 = init's return   */
0:  b       0b                      /* unreachable                    */
plugin_tramp_end:
