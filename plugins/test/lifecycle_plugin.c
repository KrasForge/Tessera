/* One fixture per lifecycle stage. Infinite loops dirty FP state as well. */
#include "plugin_abi.h"
#ifndef HANG_STAGE
#define HANG_STAGE 0
#endif
static float gain;
static void spin(void)
{
    __asm__ volatile("movi v8.16b, #0x5a" ::: "v8");
    for (;;) __asm__ volatile("nop");
}
uint32_t plugin_abi_version(void)
{
    if (HANG_STAGE == 1) spin();
    return TESSERA_PLUGIN_ABI_VERSION;
}
int plugin_init(uint32_t sr, uint32_t frames)
{
    (void)sr; (void)frames;
    if (HANG_STAGE == 2) spin();
    gain = 0.5f;
    return TESSERA_PLUGIN_OK;
}
void plugin_process_block(const float *a, const float *b, float *c, float *d, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) { c[i] = gain * a[i]; d[i] = gain * b[i]; }
}
void plugin_set_param(uint32_t id, float value)
{
    (void)id;
    if (HANG_STAGE == 3) spin();
    gain = value;
}
void plugin_destroy(void)
{
    if (HANG_STAGE == 4) spin();
}
