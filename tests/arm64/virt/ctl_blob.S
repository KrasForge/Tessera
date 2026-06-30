/* tests/arm64/virt/ctl_blob.S - embed the control-client plugin ELF
 * (Issue #28, M6).  The Makefile builds plugin_controller.elf first. */

    .section .rodata
    .balign 8

    .global ctl_elf_start
    .global ctl_elf_end
ctl_elf_start:
    .incbin "build/arm/plugin_controller.elf"
ctl_elf_end:
