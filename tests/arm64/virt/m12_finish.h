/* End the QEMU-only M12 harness through the same PSCI conduit as CPU_ON. */
#ifndef M12_FINISH_H
#define M12_FINISH_H
static inline void m12_finish(void)
{
    register unsigned long x0 __asm__("x0") = 0x84000008u; /* SYSTEM_OFF */
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
    for (;;) __asm__ volatile("wfe");
}
#endif
