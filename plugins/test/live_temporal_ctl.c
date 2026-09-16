/* Real trusted EL0 client, running simultaneously with two DSP workers. */
#include "plugin_abi.h"
#include "usermode.h"
#include "ring_contract.h"
static long set(uint64_t pid, uint64_t p, uint64_t d, uint64_t b)
{
    register uint64_t x0 __asm__("x0")=pid, x1 __asm__("x1")=p;
    register uint64_t x2 __asm__("x2")=d, x3 __asm__("x3")=b;
    register uint64_t x4 __asm__("x4")=0, x5 __asm__("x5")=0;
    register uint64_t x8 __asm__("x8")=SYS_PLUGIN_SET_CONTRACT;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8) : "memory");
    return (long)x0;
}
static long nested_load(void)
{
    register uint64_t x0 __asm__("x0")=(uint64_t)(uintptr_t)"hang-abi";
    register uint64_t x8 __asm__("x8")=SYS_PLUGIN_LOAD;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return (long)x0;
}
uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }
int plugin_init(uint32_t sr, uint32_t frames)
{
    (void)sr; (void)frames;
    volatile uint64_t *r=(volatile uint64_t *)RESULTS_VA;
    for (unsigned tries=0; tries<1000000 && r[8]<16; ++tries) {
        long result=set(r[0],r[1],r[2],r[3] - (r[8]&1u));
        if (result==0) ++r[8];
        else if (result==-5) ++r[9]; /* TC_EBUSY: wait for next frame boundary */
        else { r[10]=(uint64_t)result; return 1; }
        for (volatile unsigned delay=0; delay<100; ++delay) { }
    }
    if (r[8]!=16) return 2;
    uint64_t bits=0x400a000000000000ull, got;
    __asm__ volatile("fmov d8, %0" :: "r"(bits) : "v8");
    r[11]=(uint64_t)nested_load();
    __asm__ volatile("fmov %0, d8" : "=r"(got));
    r[12]=(got==bits); /* nested budget kill must restore caller FP state */
    return r[12] ? 0 : 3;
}
void plugin_process_block(const float *a,const float *b,float *c,float *d,uint32_t n)
{ (void)a;(void)b;(void)c;(void)d;(void)n; }
void plugin_set_param(uint32_t i,float v) { (void)i;(void)v; }
void plugin_destroy(void) { }
