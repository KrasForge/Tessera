/* plugins/example_ctl2/ctl2.c - EL0 control client for the M7 syscalls
 * (Issue #30).
 *
 * Exercises the full control surface from user space: load a plugin by name,
 * set one of its parameters, wire it to the DAC, unwire it, and unload it.
 * Each syscall's return value is recorded in a results page the kernel checks.
 *
 *   res[0] = plugin_load("pass")           expect a new pid > 0
 *   res[1] = set_param(pid, 5, 0.5f)        expect 0
 *   res[2] = graph_connect(pid, DAC=0)      expect 0
 *   res[3] = graph_disconnect(pid, DAC=0)   expect 0
 *   res[4] = plugin_unload(pid)             expect 0
 */

#include "plugin_abi.h"
#include "ring_contract.h"
#include "usermode.h"

static long sc1(long n, long a0)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}
static long sc2(long n, long a0, long a1)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}
static long sc3(long n, long a0, long a1, long a2)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate;
    (void)block_size;
    volatile long *res = (volatile long *)RESULTS_VA;
    static const char name[] = "pass";

    long pid = sc1(SYS_PLUGIN_LOAD, (long)(uintptr_t)name);
    res[0] = pid;
    res[1] = sc3(SYS_PLUGIN_SET_PARAM, pid, 5, 0x3f000000L);  /* 0.5f bits */
    res[2] = sc2(SYS_GRAPH_CONNECT, pid, GRAPH_DAC_PID);
    res[3] = sc2(SYS_GRAPH_DISCONNECT, pid, GRAPH_DAC_PID);
    res[4] = sc1(SYS_PLUGIN_UNLOAD, pid);
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    (void)in_l; (void)in_r; (void)out_l; (void)out_r; (void)n_frames;
}

void plugin_set_param(uint32_t param_id, float value) { (void)param_id; (void)value; }
void plugin_destroy(void) { }
