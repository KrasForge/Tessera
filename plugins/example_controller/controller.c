/* plugins/example_controller/controller.c - EL0 control client (Issue #28, M6)
 *
 * Stands in for the host's control thread: from user space it calls the
 * audio-graph control syscalls to wire and unwire a node at runtime, and
 * records each return value into a shared results page the kernel checks.
 *
 *   res[0] = connect(synth, DAC)          expect 0
 *   res[1] = connect(synth, DAC) again    expect an error (duplicate)
 *   res[2] = list()                       expect 1 edge
 *   res[3] = disconnect(synth, DAC)       expect 0
 *   res[4] = list()                       expect 0 edges
 */

#include "plugin_abi.h"
#include "ring_contract.h"
#include "usermode.h"

static long sc2(long n, long a0, long a1)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}

static long sc0(long n)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate;
    (void)block_size;
    volatile long *res = (volatile long *)RESULTS_VA;

    res[0] = sc2(SYS_GRAPH_CONNECT,    GRAPH_SYNTH_PID, GRAPH_DAC_PID);
    res[1] = sc2(SYS_GRAPH_CONNECT,    GRAPH_SYNTH_PID, GRAPH_DAC_PID);
    res[2] = sc0(SYS_GRAPH_LIST);
    res[3] = sc2(SYS_GRAPH_DISCONNECT, GRAPH_SYNTH_PID, GRAPH_DAC_PID);
    res[4] = sc0(SYS_GRAPH_LIST);
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    (void)in_l; (void)in_r; (void)out_l; (void)out_r; (void)n_frames;
}

void plugin_set_param(uint32_t param_id, float value) { (void)param_id; (void)value; }
void plugin_destroy(void) { }
