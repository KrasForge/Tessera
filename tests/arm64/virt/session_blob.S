.section .rodata
.balign 16
.global session_source_start,session_source_end
session_source_start: .incbin "build/arm/plugin_session_source.elf"
session_source_end:
.balign 16
.global session_gain_start,session_gain_end
session_gain_start: .incbin "build/arm/plugin_session_gain.elf"
session_gain_end:
.balign 16
.global session_hog_start,session_hog_end
session_hog_start: .incbin "build/arm/plugin_hog.elf"
session_hog_end:
.balign 16
.global session_sine_start,session_sine_end
session_sine_start: .incbin "build/arm/plugin_good.elf"
session_sine_end:
.balign 16
.global session_crash_start,session_crash_end
session_crash_start: .incbin "build/arm/plugin_session_crash.elf"
session_crash_end:
.balign 16
.global session_filter_start,session_filter_end
session_filter_start: .incbin "build/arm/plugin_effect_filter.elf"
session_filter_end:
