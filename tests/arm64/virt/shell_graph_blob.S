/* tests/arm64/virt/shell_graph_blob.S - embed the plugins the console demo
 * loads (Issue #81): a sine generator ("synth") and a gain stage ("effect").
 * The Makefile builds these ELFs first. */

    .section .rodata
    .balign 8

    .global synth_elf_start
    .global synth_elf_end
synth_elf_start:
    .incbin "build/arm/plugin_good.elf"
synth_elf_end:

    .balign 8
    .global effect_elf_start
    .global effect_elf_end
effect_elf_start:
    .incbin "build/arm/plugin_effect_filter.elf"
effect_elf_end:
