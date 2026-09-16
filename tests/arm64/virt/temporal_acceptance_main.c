/* Independent integration acceptance: real EL0 plugins and real CNTP IRQs.
 * CPU0 owns the audio cadence; CPU1 runs the production temporal_host adapter.
 * No output/diagnostics on the realtime paths. QEMU only, not hardware WCET.
 */
#include "pmm.h"
#include "pmem.h"
#include "mmu.h"
#include "vmem.h"
#include "process.h"
#include "exceptions.h"
#include "plugin_loader.h"
#include "plugin_abi.h"
#include "plugin_mgr.h"
#include "graph_control.h"
#include "ring_contract.h"
#include "temporal_host.h"
#include "usermode.h"
#include "m12_finish.h"
#include "smp.h"
#include "spsc_ring.h"
#include "audio_core.h"
#include "audio_worker.h"
#include "gic.h"
#include "timer.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
extern char good_elf_start[], good_elf_end[], blip_elf_start[], blip_elf_end[];
extern char hog_elf_start[], hog_elf_end[], crash_elf_start[], crash_elf_end[];
#define N 6u
#define BLOCKS 128u
#define HZ (RING_SR / RING_BLOCK)
#define SAMPLES (RING_BLOCK * 2u)
#define OUT_L RESULTS_VA
#define OUT_R (RESULTS_VA + RING_BLOCK * 4u)
#define IN_L RING_IN_VA
#define IN_R (RING_IN_VA + RING_BLOCK * 4u)

static graph_control_t gc;
static plugin_mgr_t pm;
static temporal_host_t host;
static audio_worker_t worker;
static audio_core_t audio;
static spsc_ring_t dac;
static int16_t dac_storage[4096], dma[SAMPLES];
static uint8_t worker_stack[32768] __attribute__((aligned(16)));
static uint64_t freq, interval;
static uint64_t sequence, underruns;
static uint32_t failures, updates, update_failures, pattern;

typedef struct {
    plugin_t *plugin;
    uint32_t pid, *out, *input;
    uint64_t published, audible, leaks, bypassed;
    int to_dac;
} node_t;
static node_t nodes[N];
static uint64_t clock_ticks(void)
{
    uint64_t t;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t) :: "memory");
    return t;
}
static int sound(const uint32_t *p)
{
    for (uint32_t i=0;i<SAMPLES;i++) if(p[i]) return 1;
    return 0;
}
static void check(int ok, const char *name)
{
    uart_printf("acceptance: %s %s\r\n",name,ok ? "PASS" : "FAIL");
    if(!ok) failures++;
}
static void finish_early(const char *name)
{
    check(0,name); uart_puts("TEMPORAL ACCEPTANCE: FAIL\r\n"); m12_finish();
}
static void published(void *ctx, uint32_t pid, enum tc_output action)
{
    node_t *n=ctx; (void)pid; n->published++;
    if(action==TC_OUTPUT_SILENCE && sound(n->out)) n->leaks++;
    if(action==TC_OUTPUT_BYPASS) {
        n->bypassed++;
        for(uint32_t i=0;i<SAMPLES;i++) if(n->out[i]!=n->input[i]) n->leaks++;
    }
    if(action==TC_OUTPUT_OK && sound(n->out)) {
        n->audible++;
        if(n->to_dac) {
            int16_t chunk[SAMPLES];
            for(uint32_t i=0;i<SAMPLES;i++) chunk[i]=(int16_t)((pattern+i)&0x7fff);
            pattern+=SAMPLES;
            if(spsc_write(&dac,chunk,SAMPLES)!=SAMPLES) n->leaks++;
        }
    }
}
static int bind_node(uint32_t i, const char *name)
{
    node_t *n=&nodes[i];
    long pid=pm_load(&pm,name);
    if(pid<=0) return 0;
    n->pid=(uint32_t)pid; n->plugin=pm_plugin(&pm,n->pid);
    uintptr_t out=phys_alloc_page_zero(), in=phys_alloc_page_zero();
    if(!out || !in) return 0;
    if(plugin_map_region(n->plugin,RESULTS_VA,out,PAGE_SIZE,VMM_READ|VMM_WRITE) ||
       plugin_map_region(n->plugin,RING_IN_VA,in,PAGE_SIZE,VMM_READ)) return 0;
    n->out=(uint32_t *)P2V(out); n->input=(uint32_t *)P2V(in);
    for(uint32_t k=0;k<RING_BLOCK;k++) {
        n->input[k]=0x3e000000u; n->input[RING_BLOCK+k]=0xbe000000u;
    }
    tc_plugin_binding_t b={
        .call={n->plugin,IN_L,IN_R,OUT_L,OUT_R,n->out,n->out+RING_BLOCK,RING_BLOCK},
        .input_left=n->input,.input_right=n->input+RING_BLOCK,
        .publish=published,.ctx=n
    };
    return plugin_call_init(n->plugin,RING_SR,RING_BLOCK)==TESSERA_PLUGIN_OK &&
           tc_host_bind(&host,&b)==TC_OK;
}
void scheduler_tick(struct trapframe *tf)
{
    (void)tf;
    if(audio.serviced>=BLOCKS) return;
    sequence++;
    /* timer_tick already advanced CVAL; this is the original release. */
    aw_kick_at(&worker,sequence,timer_deadline()-timer_interval());
    uint64_t start=clock_ticks();
    if(audio_core_fill(&audio)<SAMPLES) underruns++;
    audio_wd_account(&audio.wd,clock_ticks()-start);
    audio.serviced++;
}
static void worker_entry(void *ctx)
{
    mmu_join(); exceptions_init(); gic_cpu_init(); gic_enable_irq(TIMER_IRQ);
    aw_worker_loop(ctx);
}
void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== independent temporal-contract QEMU acceptance ===\r\n");
    pmm_init(); mmu_init(); exceptions_init();
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(freq)); interval=freq/HZ;
    gc_ring_ops_t rops={0}; gc_init(&gc,&rops); pm_init(&pm,&gc);
    pm_register_blob(&pm,"good",good_elf_start,(size_t)(good_elf_end-good_elf_start));
    pm_register_blob(&pm,"blip",blip_elf_start,(size_t)(blip_elf_end-blip_elf_start));
    pm_register_blob(&pm,"hog",hog_elf_start,(size_t)(hog_elf_end-hog_elf_start));
    pm_register_blob(&pm,"crash",crash_elf_start,(size_t)(crash_elf_end-crash_elf_start));
    tc_limits_t limits={interval,interval/10u,interval/50u};
    tc_host_init(&host,&gc,&limits,clock_ticks);
    size_t baseline=pmm_free_pages();
    const char *names[N]={"good","good","blip","hog","crash","hog"};
    for(uint32_t i=0;i<N;i++) if(!bind_node(i,names[i])) finish_early("plugin binding");
    nodes[0].to_dac=1;
    int d=gc_add_dac(&gc);
    if(d<0 || audio_graph_connect(&gc.graph,audio_graph_node_by_pid(&gc.graph,nodes[0].pid),d)<0)
        finish_early("DAC model");
    temporal_contract_t c[N]={0};
    for(uint32_t i=0;i<N;i++) {
        c[i].pid=nodes[i].pid; c[i].period=interval; c[i].deadline=interval;
        c[i].cpu_budget=interval/8u; c[i].criticality=TC_SOFT;
        c[i].overrun_policy=TC_MUTE;
    }
    c[0].criticality=TC_HARD; c[0].deadline=interval/2u;
    c[1].criticality=TC_HARD; c[1].period=interval*2u; c[1].deadline=interval/3u;
    c[2].cpu_budget=interval/6u; c[2].deadline=interval*3u/4u; c[2].overrun_policy=TC_BYPASS;
    c[3].cpu_budget=interval/6u; c[3].deadline=interval*7u/8u;
    c[3].overrun_policy=TC_MUTE_THEN_KILL; c[3].kill_after=3;
    c[5].criticality=TC_BEST_EFFORT; c[5].cpu_budget=interval-limits.job_overhead;
    check(tc_host_stage(&host,c,N)==TC_OK,"admission");
    if(failures) finish_early("admitted graph required");
    aw_init(&worker,1);
    if(tc_host_attach(&host,&worker)!=TC_OK) finish_early("exclusive worker attach");
    tc_host_bind_control(&host);
    spsc_init(&dac,dac_storage,4096);
    audio_core_init(&audio,&dac,dma,RING_BLOCK,interval/2u);
    int16_t silence[SAMPLES]={0}; spsc_write(&dac,silence,SAMPLES);
    gic_init();
    int r=smp_start_core(1,worker_entry,&worker,
                         (uint64_t)(uintptr_t)(worker_stack+sizeof worker_stack));
    uint64_t start=clock_ticks();
    while(!__atomic_load_n(&worker.online,__ATOMIC_ACQUIRE) && clock_ticks()-start<freq) { }
    if(r || !worker.online) finish_early("worker online");
    timer_init(HZ); __asm__ volatile("msr daifclr,#2");
    while(audio.serviced<BLOCKS) {
        if(audio.serviced>=32 && !updates) {
            /* A low-utilisation-looking but impossible early deadline is
             * rejected transactionally; then change a valid live budget. */
            long bad=sys_plugin_set_contract(nodes[0].pid,interval,interval/2u,
                                               interval*2u/5u,TC_HARD,0);
            if(bad!=TC_EADMISSION) update_failures++;
            long unknown=sys_plugin_set_contract(UINT32_MAX-1u,interval,interval/2u,
                                                   interval/8u,TC_HARD,0);
            if(unknown!=TC_ENODEV) update_failures++;
            long good=sys_plugin_set_contract(nodes[0].pid,interval,interval/2u,
                                                interval*3u/20u,TC_HARD,0);
            if(good!=TC_OK) update_failures++;
            updates=1;
        }
        __asm__ volatile("wfi");
    }
    timer_stop(); __asm__ volatile("msr daifset,#2");
    start=clock_ticks(); while(!aw_drained(&worker) && clock_ticks()-start<freq) { }
    int drained=aw_drained(&worker); aw_stop(&worker);
    check(drained && host.last_result==TC_OK,"worker drain");
    if(!drained) finish_early("drain required before inspect");
    check(audio.serviced==BLOCKS && !underruns && !audio.wd.overruns && !worker.overruns,
          "zero missed audio blocks");
    check(updates==1 && !update_failures && !host.scheduler.pending_ready,
          "transactional live update");
    const tc_task_state_t *v[N];
    for(uint32_t i=0;i<N;i++) {
        v[i]=tc_state(&host.scheduler,nodes[i].pid);
        if(!v[i]) finish_early("state attribution");
        uart_printf("node %u: runs=%u complete=%u budget=%u deadline=%u shed=%u killed=%u\r\n",
            (unsigned)i,(unsigned)v[i]->runs,(unsigned)v[i]->completed,
            (unsigned)v[i]->budget_overruns,(unsigned)v[i]->deadline_misses,
            (unsigned)v[i]->shed,(unsigned)v[i]->killed);
        check(!nodes[i].leaks && nodes[i].published==BLOCKS,"full output publication");
    }
    check(v[0]->completed==BLOCKS && !v[0]->budget_overruns && !v[0]->deadline_misses &&
          nodes[0].audible==BLOCKS,"hard audio survives");
    check(v[1]->completed==BLOCKS/2u && !v[1]->deadline_misses,"independent half-rate period");
    check(v[2]->budget_overruns==2 && nodes[2].bypassed==2 && !v[2]->killed &&
          v[2]->completed==BLOCKS-2u,"transient bypass and recovery");
    check(v[3]->budget_overruns==3 && v[3]->killed && v[3]->runs==6 &&
          nodes[3].plugin->proc->state==PROC_KILLED,"three-strike actual termination");
    check(plugin_call_block(nodes[3].plugin,IN_L,IN_R,OUT_L,OUT_R,RING_BLOCK)==-1,
          "terminated plugin cannot run");
    check(v[4]->faults==1 && v[4]->killed,"fault contained without RT UART");
    check(!v[5]->runs && v[5]->shed==BLOCKS,"best-effort overload shed");
    /* The never-scheduled second hog now exercises an ABSOLUTE deadline
     * shorter than its relative CPU budget, on idle CPU0 with cadence off. */
    budget_plugin_call_t call={nodes[5].plugin,IN_L,IN_R,OUT_L,OUT_R,
                               nodes[5].out,nodes[5].out+RING_BLOCK,RING_BLOCK};
    for(unsigned i=0;i<3;i++) plugin_call_block(nodes[5].plugin,IN_L,IN_R,OUT_L,OUT_R,RING_BLOCK);
    uint64_t elapsed=0;
    long stop=budget_plugin_invoke(&call,interval/2u,clock_ticks()+interval/6u,&elapsed);
    check(stop==BUDGET_DEADLINE && elapsed<interval/2u,"absolute deadline IRQ preemption");
    tc_host_bind_control(NULL);
    for(uint32_t i=0;i<N;i++) {
        check(tc_host_unbind(&host,nodes[i].pid)==TC_OK,"unbind before reclaim");
        check(pm_unload(&pm,nodes[i].pid)==PM_OK,"unload");
    }
    check(host.bindings==0 && host.desired_count==0 && !host.scheduler.active_valid,
          "no stale temporal bindings");
    check(pmm_free_pages()==baseline,"no frame leaks");
    uart_printf("audio: blocks=%u underruns=%u watchdog=%u worker_skips=%u\r\n",
        (unsigned)audio.serviced,(unsigned)underruns,(unsigned)audio.wd.overruns,
        (unsigned)worker.overruns);
    uart_puts(failures ? "TEMPORAL ACCEPTANCE: FAIL\r\n" : "TEMPORAL ACCEPTANCE: PASS\r\n");
    m12_finish();
}
