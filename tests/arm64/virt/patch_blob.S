/* tests/arm64/virt/patch_blob.S - embed the plugins used by the patch
 * persistence harness (Issue #40).  The Makefile builds these ELFs first. */

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
    .incbin "build/arm/plugin_effect.elf"
effect_elf_end:
