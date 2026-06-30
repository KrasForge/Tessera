/* tests/arm64/virt/plugin_blob.S - embed the prebuilt plugin ELFs as a
 * stand-in ramdisk (Issue #24, M5).
 *
 * The plugin loader reads a plugin from memory, so for the QEMU harness we
 * incbin the cross-compiled plugin executables directly into the test image.
 * The Makefile builds build/arm/plugin_pass.elf and build/arm/plugin_evil.elf
 * before assembling this file.
 */

    .section .rodata
    .balign 8

    .global pass_elf_start
    .global pass_elf_end
pass_elf_start:
    .incbin "build/arm/plugin_pass.elf"
pass_elf_end:

    .balign 8
    .global evil_elf_start
    .global evil_elf_end
evil_elf_start:
    .incbin "build/arm/plugin_evil.elf"
evil_elf_end:
