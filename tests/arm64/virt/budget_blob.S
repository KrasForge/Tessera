/* tests/arm64/virt/budget_blob.S - embed the issue #78 demo plugins.
 * The Makefile builds these ELFs first. */

    .section .rodata
    .balign 8

    .global good_elf_start
    .global good_elf_end
good_elf_start:
    .incbin "build/arm/plugin_good.elf"
good_elf_end:

    .balign 8
    .global blip_elf_start
    .global blip_elf_end
blip_elf_start:
    .incbin "build/arm/plugin_blip.elf"
blip_elf_end:

    .balign 8
    .global hog_elf_start
    .global hog_elf_end
hog_elf_start:
    .incbin "build/arm/plugin_hog.elf"
hog_elf_end:
