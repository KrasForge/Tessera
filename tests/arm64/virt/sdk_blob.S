/* tests/arm64/virt/sdk_blob.S - embed the SDK example plugin (Issue #38).
 * The Makefile builds sine_plugin.elf via the SDK's own Makefile first. */

    .section .rodata
    .balign 8

    .global sdk_sine_elf_start
    .global sdk_sine_elf_end
sdk_sine_elf_start:
    .incbin "sdk/examples/sine_plugin/sine_plugin.elf"
sdk_sine_elf_end:
