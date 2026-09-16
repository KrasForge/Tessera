/* Trusted EL0 control client: exercises the real six-register contract SVC. */
#include "plugin_abi.h"
#include "ring_contract.h"
#include "usermode.h"
static long contract(uint64_t pid, uint64_t period, uint64_t deadline,
                     uint64_t budget, uint64_t flags, uint64_t argument)
{
    register uint64_t x0 __asm__("x0") = pid;
    register uint64_t x1 __asm__("x1") = period;
    register uint64_t x2 __asm__("x2") = deadline;
    register uint64_t x3 __asm__("x3") = budget;
    register uint64_t x4 __asm__("x4") = flags;
    register uint64_t x5 __asm__("x5") = argument;
    register uint64_t x8 __asm__("x8") = SYS_PLUGIN_SET_CONTRACT;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                     "r"(x4), "r"(x5), "r"(x8) : "memory");
    return (long)x0;
}
uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }
int plugin_init(uint32_t sr, uint32_t frames)
{
    (void)sr; (void)frames;
    volatile uint64_t *r = (volatile uint64_t *)RESULTS_VA;
    r[16] = contract(0xfffffffeu, r[1], r[2], r[3], r[4], 0);
    r[17] = contract(r[0], r[1], r[2], r[3], 1ull << 32, 0);
    r[18] = contract(r[0], 0, r[2], r[3], r[4], 0);
    r[19] = contract(1ull << 40, r[1], r[2], r[3], r[4], 0);
    r[20] = contract(r[0], r[1], r[2], r[3], r[4], 0);
    r[21] = contract(r[0], r[1], r[2], r[3], r[4], 0);
    return TESSERA_PLUGIN_OK;
}
void plugin_process_block(const float *a, const float *b, float *c, float *d, uint32_t n)
{ (void)a; (void)b; (void)c; (void)d; (void)n; }
void plugin_set_param(uint32_t id, float v) { (void)id; (void)v; }
void plugin_destroy(void) { }
