/* tests/arm64/virt/ring_blob.S - embed the shared-ring example plugin ELFs
 * (Issue #25, M5).  The Makefile builds these before assembling this file. */

    .section .rodata
    .balign 8

    .global producer_elf_start
    .global producer_elf_end
producer_elf_start:
    .incbin "build/arm/plugin_producer.elf"
producer_elf_end:

    .balign 8
    .global crasher_elf_start
    .global crasher_elf_end
crasher_elf_start:
    .incbin "build/arm/plugin_crasher.elf"
crasher_elf_end:
