/* boot/start.S - Tessera AArch64 entry point (skeleton; issue #1)
 *
 * Target: Raspberry Pi CM4 / Pi 4 (BCM2711, Cortex-A72).
 *
 * The VideoCore firmware enters here (physical 0x80000) on core 0 at EL2
 * with the MMU off and caches in an undefined state. This skeleton does the
 * bare minimum needed for a valid, linkable image:
 *
 *   1. Park the secondary cores (1-3) in a low-power wait loop.
 *   2. Set up the initial stack pointer from the linker script.
 *   3. Branch to the C entry point kmain().
 *
 * The full bring-up - EL2->EL1 transition, BSS clear, and cache/MMU
 * configuration - is completed in issue #2 (ARM bare-metal boot). The
 * linker already exports __bss_start / __bss_end / __stack_top for it.
 */

    .section .text.boot, "ax"
    .global _start

_start:
    /* Only the primary core (Aff0 == 0) continues; the rest wait. */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, primary_core

park_secondary:
    wfe
    b       park_secondary

primary_core:
    /* Initialise the stack pointer (top of the .stack region). */
    ldr     x0, =__stack_top
    mov     sp, x0

    /* Enter C. kmain() must not return. */
    bl      kmain

    /* Safety net: if kmain() ever returns, park this core. */
halt:
    wfe
    b       halt
