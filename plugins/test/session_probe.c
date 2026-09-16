/* Deterministic stereo reference DSP + bounded-work capacity probe.
 * Only the test application enables read-only EL0 virtual-counter access. */
#include "plugin_abi.h"
#ifndef PROBE_GAIN
#define PROBE_GAIN 0
#endif
static float value = 0.5f;
static uint64_t busy_ticks;
#ifdef PROBE_CRASH
static uint32_t blocks;
#endif
static uint64_t counter(void)
{
    uint64_t n;
    __asm__ volatile("isb; mrs %0,cntvct_el0" : "=r"(n)::"memory");
    return n;
}
uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }
int plugin_init(uint32_t sr, uint32_t frames)
{
    (void)sr;
    (void)frames;
    value = 0.5f;
    busy_ticks = 0;
#ifdef PROBE_CRASH
    blocks = 0;
#endif
    return 0;
}
void plugin_set_param(uint32_t id, float v)
{
    if (id == 0)
        value = v;
    if (id == 1) {
        uint64_t hz;
        __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(hz));
        if (v >= 0 && v <= 10000)
            busy_ticks = ((uint64_t)(uint32_t)v * hz) / 1000000u;
    }
}
void plugin_process_block(const float *il, const float *ir, float *ol, float *or_, uint32_t n)
{
#ifdef PROBE_CRASH
    if (++blocks == 4) {
        /* Produce partial scratch output before a real EL0 null write. */
        if (n) ol[0] = 0.75f;
        __asm__ volatile("mov x9, xzr; str wzr, [x9]" ::: "x9", "memory");
    }
#endif
    if (busy_ticks) {
        uint64_t start = counter();
        while (counter() - start < busy_ticks)
            __asm__ volatile("yield");
    }
    for (uint32_t i = 0; i < n; ++i) {
#if PROBE_GAIN
        ol[i] = il[i] * value;
        or_[i] = ir[i] * value;
#else
        (void)il;
        (void)ir;
        ol[i] = value;
        or_[i] = -value;
#endif
    }
}
void plugin_destroy(void) {}
