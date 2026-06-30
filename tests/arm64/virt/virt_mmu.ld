/* tests/arm64/virt/virt_mmu.ld — linker script for the full-stack fault
 * harness, which brings up the real pmm/mmu on the QEMU 'virt' board.
 *
 * Defines __kernel_start/__kernel_end (used by pmm to reserve the image) and
 * __bss_start/__bss_end/__stack_top (used by start_virt.S).  Loaded above the
 * DTB at the bottom of virt RAM (0x40000000). */

ENTRY(_vstart)

SECTIONS
{
    . = 0x40200000;
    __kernel_start = .;

    .text : ALIGN(0x800) { KEEP(*(.text.boot)) *(.text .text.*) }
    .rodata : ALIGN(0x10) { *(.rodata .rodata.*) }
    .data   : ALIGN(0x10) { *(.data .data.*) }
    .bss (NOLOAD) : ALIGN(0x10) {
        __bss_start = .;
        *(COMMON) *(.bss .bss.*)
        . = ALIGN(0x10);
        __bss_end = .;
    }

    . = ALIGN(0x1000);
    . += 0x20000;
    __stack_top = .;

    . = ALIGN(0x1000);
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) *(.ARM.attributes) }
}
