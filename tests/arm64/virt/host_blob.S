/* tests/arm64/virt/host_blob.S - embed the sine plugin ELFs (Issue #26, M5).
 * The Makefile builds plugin_sine.elf and plugin_sine_crash.elf first. */

    .section .rodata
    .balign 8

    .global sine_elf_start
    .global sine_elf_end
sine_elf_start:
    .incbin "build/arm/plugin_sine.elf"
sine_elf_end:

    .balign 8
    .global sine_crash_elf_start
    .global sine_crash_elf_end
sine_crash_elf_start:
    .incbin "build/arm/plugin_sine_crash.elf"
sine_crash_elf_end:
