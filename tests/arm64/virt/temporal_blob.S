.section .rodata
.balign 16
.global good_elf_start, good_elf_end
good_elf_start: .incbin "build/arm/plugin_good.elf"
good_elf_end:
.balign 16
.global blip_elf_start, blip_elf_end
blip_elf_start: .incbin "build/arm/plugin_blip.elf"
blip_elf_end:
.balign 16
.global hog_elf_start, hog_elf_end
hog_elf_start: .incbin "build/arm/plugin_hog.elf"
hog_elf_end:
.balign 16
.global temporal_ctl_start, temporal_ctl_end
temporal_ctl_start: .incbin "build/arm/plugin_temporal_ctl.elf"
temporal_ctl_end:

.balign 16
.global trampoline_svc_start, trampoline_svc_end
trampoline_svc_start: .incbin "build/arm/plugin_trampoline_svc.elf"
trampoline_svc_end:
