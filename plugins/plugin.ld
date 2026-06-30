/* plugins/plugin.ld - link a Tessera plugin as an isolated AArch64 executable
 * (Issue #24, M5).
 *
 * Plugins are linked at USER_VA_BASE (512 GiB, TTBR0 L0 index 1), the start of
 * per-process user space, with each output section page-aligned so the loader
 * can map one PT_LOAD segment per permission class (RX text, R rodata, RW
 * data/bss).  Entry is plugin_init purely for tidiness; the host actually
 * dispatches by resolving symbols, not via e_entry.
 */

ENTRY(plugin_init)

SECTIONS
{
    . = 0x8000000000;

    .text   : ALIGN(0x1000) { *(.text .text.*) }
    .rodata : ALIGN(0x1000) { *(.rodata .rodata.*) }
    .data   : ALIGN(0x1000) { *(.data .data.*) }
    .bss    : ALIGN(0x1000) { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) *(.ARM.attributes) }
}
