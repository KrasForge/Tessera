/* tests/arm64/virt/hot_reload_blob.S - plugin image for the hot-reload demo
 * (Theme A: reliability).  The reloaded plugin is the M6 reference low-pass;
 * the harness loads it twice, as two isolated instances, and swaps live from
 * the first to the second.  The Makefile builds the ELF first. */

    .section .rodata
    .balign 8

    .global effect_elf_start
    .global effect_elf_end
effect_elf_start:
    .incbin "build/arm/plugin_effect_filter.elf"
effect_elf_end:
