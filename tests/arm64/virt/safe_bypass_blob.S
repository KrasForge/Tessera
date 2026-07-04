/* tests/arm64/virt/safe_bypass_blob.S - plugins for the safe-mode bypass demo
 * (Theme A: reliability).  The effect is the M6 reference low-pass; the fatal
 * fault is the M8 crash plugin (a null dereference in process_block).  The
 * Makefile builds both ELFs first. */

    .section .rodata
    .balign 8

    .global effect_elf_start
    .global effect_elf_end
effect_elf_start:
    .incbin "build/arm/plugin_effect_filter.elf"
effect_elf_end:

    .balign 8
    .global crash_elf_start
    .global crash_elf_end
crash_elf_start:
    .incbin "build/arm/plugin_crash.elf"
crash_elf_end:
