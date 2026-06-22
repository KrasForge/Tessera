/* arch/arm64/main.c - Tessera AArch64 C entry point (skeleton; issue #1)
 *
 * Minimal placeholder so the AArch64 toolchain has C to compile and the
 * linker produces a complete kernel image. It is reached from boot/start.S
 * after the stack is set up.
 *
 * Real initialisation arrives in later milestones:
 *   - issue #2: EL2->EL1 drop, BSS clear, CPU/clock init
 *   - issue #3: UART console + boot banner (replaces the wait loop below)
 *   - issue #4+: GPIO/LED, MMU, scheduler, ...
 */

void kmain(void)
{
    /* Nothing to do yet. Park the core in a low-power wait loop so the
     * image has well-defined behaviour when run on hardware or in QEMU. */
    for (;;) {
        __asm__ volatile("wfe");
    }
}
