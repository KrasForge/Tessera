/* boot/start.S — Tessera AArch64 bare-metal entry point (Issue #2)
 *
 * Target: Raspberry Pi CM4 / Pi 4 (BCM2711, Cortex-A72).
 *
 * The VideoCore firmware loads kernel8.img at physical 0x80000 and enters
 * here on core 0.  Entry privilege level is EL2 on real hardware; some QEMU
 * configurations start at EL1 — both cases are handled by checking CurrentEL.
 *
 * Sequence:
 *   1. Park secondary cores (cores 1-3) in a WFE spin.
 *   2. If already at EL1, jump straight to el1_entry.
 *   3. At EL2: set HCR_EL2.RW (AArch64 EL1), enable counter access,
 *      configure SPSR_EL2 and ELR_EL2, then ERET to EL1.
 *   4. At EL1: disable MMU / D-cache / I-cache via SCTLR_EL1.
 *   5. Initialise SP_EL1 from __stack_top (defined in the linker script).
 *   6. Zero BSS from __bss_start to __bss_end using 8-byte stores.
 *   7. Branch to kmain() in C — must not return.
 */

    .section .text.boot, "ax"
    .global _start

_start:
    /* Park all cores except core 0 (Aff0 field in MPIDR_EL1). */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, primary_core

park_secondary:
    wfe
    b       park_secondary

primary_core:
    /* Read the current exception level.  Shift right 2 to get the numeric
     * EL value from CurrentEL[3:2]. */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #1
    b.eq    el1_entry               /* Already at EL1, skip privilege drop */

    /* --------------------------------------------------------------------- */
    /* EL2 configuration — executed only when firmware enters at EL2          */
    /* --------------------------------------------------------------------- */

    /* HCR_EL2.RW = 1: EL1 is AArch64 (not AArch32). */
    mrs     x0, hcr_el2
    orr     x0, x0, #(1 << 31)
    msr     hcr_el2, x0

    /* Allow EL1 / EL0 to access the physical counter register (CNTPCT_EL0).
     * This is required for the busy-wait delay loops in drivers. */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #0x3            /* EL1PCTEN | EL1PCEN */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr        /* Zero the virtual counter offset */

    /* SPSR_EL2: mode EL1h (SP_EL1 selected, 0b0101), DAIF all masked. */
    mov     x0, #0x3C5
    msr     spsr_el2, x0

    /* ELR_EL2: address returned to after ERET. */
    adr     x0, el1_entry
    msr     elr_el2, x0

    /* Drop to EL1. */
    eret

    /* --------------------------------------------------------------------- */
    /* EL1 entry — reached either via ERET above or directly from firmware    */
    /* --------------------------------------------------------------------- */
el1_entry:
    /* Ensure MMU, D-cache, and I-cache are off before we write data
     * structures or use the stack (caches must be off to guarantee clean
     * BSS zeroing). */
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 0)      /* M:  disable MMU            */
    bic     x0, x0, #(1 << 2)      /* C:  disable D-cache        */
    bic     x0, x0, #(1 << 12)     /* I:  disable I-cache        */
    msr     sctlr_el1, x0
    isb

    /* Set up SP_EL1 (stack grows downward from __stack_top). */
    ldr     x0, =__stack_top
    mov     sp, x0

    /* Zero the BSS section.  __bss_end is 16-byte aligned (linker script),
     * so 8-byte stores always stay within bounds. */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
bss_loop:
    cmp     x0, x1
    b.ge    bss_done
    str     xzr, [x0], #8
    b       bss_loop
bss_done:

    /* Branch to the C entry point.  kmain() is not expected to return. */
    bl      kmain

    /* Safety halt: park this core if kmain() ever returns. */
halt:
    wfe
    b       halt
