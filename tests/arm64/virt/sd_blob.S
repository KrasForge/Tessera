/* tests/arm64/virt/sd_blob.S - embed the M8 SD-loader test plugins
 * (Issue #34).  The Makefile builds these ELFs first. */

    .section .rodata
    .balign 8

    .global pass_elf_start
    .global pass_elf_end
pass_elf_start:
    .incbin "build/arm/plugin_pass.elf"
pass_elf_end:

    .balign 8
    .global badabi_elf_start
    .global badabi_elf_end
badabi_elf_start:
    .incbin "build/arm/plugin_badabi.elf"
badabi_elf_end:

    .balign 8
    .global badimport_elf_start
    .global badimport_elf_end
badimport_elf_start:
    .incbin "build/arm/plugin_badimport.elf"
badimport_elf_end:
