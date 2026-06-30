/* tests/arm64/virt/sandbox_blob.S - embed the M8 sandbox test plugins
 * (Issue #35).  The Makefile builds these ELFs first. */

    .section .rodata
    .balign 8

    .global pass_elf_start
    .global pass_elf_end
pass_elf_start:
    .incbin "build/arm/plugin_pass.elf"
pass_elf_end:

    .balign 8
    .global sbsvc_elf_start
    .global sbsvc_elf_end
sbsvc_elf_start:
    .incbin "build/arm/plugin_sbsvc.elf"
sbsvc_elf_end:

    .balign 8
    .global sbmem_elf_start
    .global sbmem_elf_end
sbmem_elf_start:
    .incbin "build/arm/plugin_sbmem.elf"
sbmem_elf_end:
