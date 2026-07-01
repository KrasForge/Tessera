/* sdk/tessera.ld - link a Tessera plugin as an isolated AArch64 executable
 * (Issue #38).
 *
 * Links the plugin at the Tessera user base (0x8000000000, the start of
 * per-process user space) and places each section in a SEPARATE, page-aligned
 * PT_LOAD segment by permission class:
 *
 *     text   -> R + X   (code)
 *     rodata -> R       (constants)
 *     data   -> R + W   (data + bss)
 *
 * so no page is ever both writable and executable (W^X).  The host maps one
 * segment per permission class and the sandbox audit stays clean.  Entry is
 * plugin_init purely for tidiness; the host dispatches by resolving symbols,
 * not via e_entry.
 */

ENTRY(plugin_init)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R E */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R W */
}

SECTIONS
{
    . = 0x8000000000;

    .text   : ALIGN(0x1000) { *(.text .text.*) }            :text
    .rodata : ALIGN(0x1000) { *(.rodata .rodata.*) }        :rodata
    .data   : ALIGN(0x1000) { *(.data .data.*) }            :data
    .bss    : ALIGN(0x1000) { *(.bss .bss.*) *(COMMON) }    :data

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) *(.ARM.attributes) }
}
