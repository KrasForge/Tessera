/* tests/arm64/virt/graph_blob.S - embed the synth and effect plugin ELFs
 * (Issue #27, M6).  The Makefile builds these before assembling this file. */

    .section .rodata
    .balign 8

    .global synth_elf_start
    .global synth_elf_end
synth_elf_start:
    .incbin "build/arm/plugin_sine.elf"
synth_elf_end:

    .balign 8
    .global effect_elf_start
    .global effect_elf_end
effect_elf_start:
    .incbin "build/arm/plugin_effect.elf"
effect_elf_end:
