/* tests/arm64/virt/resilience_blob.S - embed the resilience-demo plugins
 * (Issue #36, M8; hog added by issue #79, M12).  The Makefile builds these
 * ELFs first. */

    .section .rodata
    .balign 8

    .global good_elf_start
    .global good_elf_end
good_elf_start:
    .incbin "build/arm/plugin_good.elf"
good_elf_end:

    .balign 8
    .global crash_elf_start
    .global crash_elf_end
crash_elf_start:
    .incbin "build/arm/plugin_crash.elf"
crash_elf_end:

    .balign 8
    .global evil_elf_start
    .global evil_elf_end
evil_elf_start:
    .incbin "build/arm/plugin_evil2.elf"
evil_elf_end:

    .balign 8
    .global hog_elf_start
    .global hog_elf_end
hog_elf_start:
    .incbin "build/arm/plugin_hog.elf"
hog_elf_end:
