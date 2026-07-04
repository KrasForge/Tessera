/* tests/arm64/virt/mem_quota_blob.S - plugins for the memory-quota demo
 * (Theme A: reliability).  The effect is the M6 reference low-pass (a small
 * footprint); the greedy plugin declares a large .bss.  The Makefile builds
 * both ELFs first. */

    .section .rodata
    .balign 8

    .global effect_elf_start
    .global effect_elf_end
effect_elf_start:
    .incbin "build/arm/plugin_effect_filter.elf"
effect_elf_end:

    .balign 8
    .global greedy_elf_start
    .global greedy_elf_end
greedy_elf_start:
    .incbin "build/arm/plugin_greedy.elf"
greedy_elf_end:
