/* tests/arm64/virt/shell_patch_blob.S - plugins for the console patch demo
 * (Issue #82): the SDK sine synth (frequency settable via param 0) and the
 * M6 state-variable low-pass filter.  The Makefile builds these ELFs first. */

    .section .rodata
    .balign 8

    .global synth_elf_start
    .global synth_elf_end
synth_elf_start:
    .incbin "sdk/examples/sine_plugin/sine_plugin.elf"
synth_elf_end:

    .balign 8
    .global effect_elf_start
    .global effect_elf_end
effect_elf_start:
    .incbin "build/arm/plugin_effect_filter.elf"
effect_elf_end:
