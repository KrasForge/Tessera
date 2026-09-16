    .section .rodata
    .balign 16
    .global rt_good_start, rt_good_end
rt_good_start: .incbin "build/arm/plugin_good.elf"
rt_good_end:
    .balign 16
    .global rt_hog_start, rt_hog_end
rt_hog_start: .incbin "build/arm/plugin_hog.elf"
rt_hog_end:
    .balign 16
    .global rt_ctl_start, rt_ctl_end
rt_ctl_start: .incbin "build/arm/plugin_live_temporal_ctl.elf"
rt_ctl_end:
    .balign 16
    .global rt_life0_start, rt_life0_end
rt_life0_start: .incbin "build/arm/plugin_lifecycle_0.elf"
rt_life0_end:
    .balign 16
    .global rt_life1_start, rt_life1_end
rt_life1_start: .incbin "build/arm/plugin_lifecycle_1.elf"
rt_life1_end:
    .balign 16
    .global rt_life2_start, rt_life2_end
rt_life2_start: .incbin "build/arm/plugin_lifecycle_2.elf"
rt_life2_end:
    .balign 16
    .global rt_life3_start, rt_life3_end
rt_life3_start: .incbin "build/arm/plugin_lifecycle_3.elf"
rt_life3_end:
    .balign 16
    .global rt_life4_start, rt_life4_end
rt_life4_start: .incbin "build/arm/plugin_lifecycle_4.elf"
rt_life4_end:
