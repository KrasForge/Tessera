/* tests/arm64/virt/graph_input_blob.S - the filter plugin the input-node demo
 * runs (Issue #84): the M6 state-variable low-pass.  The Makefile builds it
 * first. */

    .section .rodata
    .balign 8

    .global effect_elf_start
    .global effect_elf_end
effect_elf_start:
    .incbin "build/arm/plugin_effect_filter.elf"
effect_elf_end:
