/* arch/arm64/smp_entry.S - secondary-core trampoline (Issue #21, M4)
 *
 * PSCI CPU_ON enters here on the freshly powered core, at the highest
 * implemented EL (EL2 on QEMU 'virt' and the Pi) with the MMU off and no
 * stack.  We drop to EL1, disable the MMU/caches, install this core's stack
 * (g_smp_stack[cpu], populated by smp_start_core before the CPU_ON), and call
 * smp_secondary_main() in C.
 */

    .section .text
    .global smp_secondary_entry
smp_secondary_entry:
    /* If we came up at EL2, drop to EL1; otherwise continue. */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    1f

    mov     x0, #(1 << 31)          /* HCR_EL2.RW = AArch64 EL1 */
    msr     hcr_el2, x0
    mrs     x0, cnthctl_el2
    orr     x0, x0, #0x3            /* EL1 access to the physical counter */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #0x3C5              /* EL1h, DAIF masked */
    msr     spsr_el2, x0
    adr     x0, 1f
    msr     elr_el2, x0
    eret

1:  /* EL1: ensure MMU, D-cache, I-cache are off. */
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 0)
    bic     x0, x0, #(1 << 2)
    bic     x0, x0, #(1 << 12)
    msr     sctlr_el1, x0
    isb

    /* Install this core's stack: g_smp_stack[mpidr_aff0]. */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    ldr     x1, =g_smp_stack
    ldr     x2, [x1, x0, lsl #3]
    mov     sp, x2

    bl      smp_secondary_main
2:  wfe
    b       2b
