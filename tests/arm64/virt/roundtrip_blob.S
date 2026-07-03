/* tests/arm64/virt/roundtrip_blob.S - the reference low-pass filter plugin the
 * round-trip loopback harness runs (Issue #85): the M6 state-variable low-pass.
 * The Makefile builds the plugin ELF first. */

    .section .rodata
    .balign 8

    .global effect_elf_start
    .global effect_elf_end
effect_elf_start:
    .incbin "build/arm/plugin_effect_filter.elf"
effect_elf_end:
