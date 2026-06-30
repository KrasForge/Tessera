/* tests/arm64/virt/start_virt.S — minimal entry for the QEMU 'virt'
 * exception harness: drop to EL1 if needed, set the stack, clear BSS, and
 * call test_main(). */

    .section .text.boot, "ax"
    .global _vstart
_vstart:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    at_el1

    /* EL2 -> EL1 */
    mov     x0, #(1 << 31)          /* HCR_EL2.RW = AArch64 EL1 */
    msr     hcr_el2, x0
    mov     x0, #0x3C5              /* EL1h, DAIF masked        */
    msr     spsr_el2, x0
    adr     x0, at_el1
    msr     elr_el2, x0
    eret

at_el1:
    ldr     x0, =__stack_top
    mov     sp, x0

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
clear_bss:
    cmp     x0, x1
    b.ge    enter_c
    str     xzr, [x0], #8
    b       clear_bss

enter_c:
    bl      test_main
hang:
    wfe
    b       hang
