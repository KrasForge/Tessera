/* tests/arm64/virt/control_blob.S - embed the M7 control test plugins
 * (Issue #30).  The Makefile builds these ELFs first. */

    .section .rodata
    .balign 8

    .global pass_elf_start
    .global pass_elf_end
pass_elf_start:
    .incbin "build/arm/plugin_pass.elf"
pass_elf_end:

    .balign 8
    .global echo_elf_start
    .global echo_elf_end
echo_elf_start:
    .incbin "build/arm/plugin_echo.elf"
echo_elf_end:

    .balign 8
    .global ctl2_elf_start
    .global ctl2_elf_end
ctl2_elf_start:
    .incbin "build/arm/plugin_ctl2.elf"
ctl2_elf_end:
