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
    .balign 8
    .global crash_elf_start
    .global crash_elf_end
crash_elf_start:
    .incbin "build/arm/plugin_crash.elf"
crash_elf_end:
