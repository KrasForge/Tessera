/* Tessera AArch64 kernel linker script - Issue #1
 *
 * Target: Raspberry Pi CM4 / Pi 4 (BCM2711, Cortex-A72, ARMv8-A).
 *
 * The VideoCore firmware loads a 64-bit kernel image (kernel8.img) at
 * physical address 0x80000 and enters it on core 0 with the MMU off.
 * The first instruction of the raw image must therefore be _start, so
 * the boot text (.text.boot) is placed first below.
 *
 * This replaces the x86 scripts (kernel/kernel.ld, boot/boot.ld) for the
 * ARM target; those remain in use by the legacy x86 build.
 */

ENTRY(_start)

/* Pi firmware load address for 64-bit kernel images. */
KERNEL_LOAD_ADDR = 0x80000;

/* Size of the initial kernel stack (grows down from __stack_top). */
STACK_SIZE = 0x10000; /* 64 KiB */

SECTIONS
{
    . = KERNEL_LOAD_ADDR;
    __kernel_start = .;

    .text : ALIGN(0x1000)
    {
        KEEP(*(.text.boot))   /* _start must be the very first byte */
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000)
    {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(0x1000)
    {
        *(.data .data.*)
    }

    /* Zero-initialised data. Marked NOLOAD so it occupies no space in the
     * raw image; the boot code is responsible for clearing it (issue #2). */
    .bss (NOLOAD) : ALIGN(0x1000)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(0x10);
        __bss_end = .;
    }

    /* Initial stack. */
    .stack (NOLOAD) : ALIGN(0x1000)
    {
        __stack_bottom = .;
        . += STACK_SIZE;
        __stack_top = .;
    }

    __kernel_end = .;

    /* Drop host-toolchain metadata we do not need on bare metal. */
    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.ARM.attributes)
    }
}
