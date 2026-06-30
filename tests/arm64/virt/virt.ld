/* tests/arm64/virt/virt.ld — linker script for the QEMU 'virt' exception
 * harness.  Loaded above the DTB that QEMU places at the bottom of RAM
 * (0x40000000), so we link at 0x40200000.  Entry is _vstart. */

ENTRY(_vstart)

SECTIONS
{
    . = 0x40200000;
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
    . += 0x10000;
    __stack_top = .;

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) *(.ARM.attributes) }
}
