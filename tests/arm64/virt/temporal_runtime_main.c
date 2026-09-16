/* Four-core managed-runtime acceptance: DSP, live control and lifecycle faults. */
#include "temporal_runtime.h"
#include "pmm.h"
#include "pmem.h"
#include "mmu.h"
#include "vmem.h"
#include "exceptions.h"
#include "plugin_abi.h"
#include "ring_contract.h"
#include "m12_finish.h"
#include "smp.h"
#include "gic.h"
#include "timer.h"
#include "uart_pl011.h"
#include "spsc_ring.h"
#include "audio_core.h"
#include "usermode.h"
#include <stdint.h>
#include <stddef.h>
void uart_virt_init(void);
extern char rt_good_start[],rt_good_end[],rt_hog_start[],rt_hog_end[],rt_ctl_start[],rt_ctl_end[];
#define LIFE_EXTERN(i) extern char rt_life##i##_start[],rt_life##i##_end[];
LIFE_EXTERN(0) LIFE_EXTERN(1) LIFE_EXTERN(2) LIFE_EXTERN(3) LIFE_EXTERN(4)
static void *life_start[]={rt_life0_start,rt_life1_start,rt_life2_start,rt_life3_start,rt_life4_start};
static void *life_end[]={rt_life0_end,rt_life1_end,rt_life2_end,rt_life3_end,rt_life4_end};
static const char *life_name[]={"gain","hang-abi","hang-init","hang-param","hang-destroy"};
#define FRAMES RING_BLOCK
#define SAMPLES (FRAMES*2u)
#define HZ 200u
static uint64_t frequency,period,sequence;
static uint32_t target=128;
static uint64_t clock_now(void) { uint64_t v; __asm__ volatile("isb; mrs %0,cntpct_el0":"=r"(v)::"memory"); return v; }
static void check(int good,const char *message)
{
    if(good) return;
    uart_printf("RUNTIME: FAIL (%s)\r\n",message); m12_finish();
    for(;;) __asm__ volatile("wfe");
}
static graph_control_t graph1,graph3;
static plugin_mgr_t manager1,manager3,probe_manager;
static temporal_runtime_t runtime1,runtime3;
static audio_worker_t worker1,worker3;
static uint8_t stacks[3][32768] __attribute__((aligned(16)));
static plugin_t control, cadence_probe;
static uint32_t cadence_probe_done;
static volatile uint64_t *control_result;
static uint32_t control_done,control_ok,probe_ok,nested_count;
static spsc_ring_t dac;
static int16_t dac_storage[4096],dma[SAMPLES];
static audio_core_t audio;
static uint64_t underruns;
static uint32_t ring_used[32],ring_live;
static void *ring_new(void *ctx)
{
    (void)ctx; for(unsigned i=0;i<32;++i) if(!ring_used[i]) { ring_used[i]=1; ++ring_live; return &ring_used[i]; }
    return NULL;
}
static void ring_free(void *ctx,void *ring) { (void)ctx; if(*(uint32_t *)ring) { *(uint32_t *)ring=0; --ring_live; } }
static int ring_map(void *ctx,uint32_t pid,void *ring,int input)
{ (void)ctx;(void)pid;(void)ring;(void)input;return 0; }
static void ring_unmap(void *ctx,uint32_t pid,void *ring,int input)
{ (void)ctx;(void)pid;(void)ring;(void)input; }
typedef struct io_context io_context_t;
typedef struct { io_context_t *owner; uint32_t used,pid,calls,silent,invalid; uint32_t *in,*out; } io_node_t;
struct io_context { plugin_mgr_t *manager; uint32_t sink,half_check; io_node_t nodes[16]; };
static io_context_t io1,io3;
static io_node_t *io_find(io_context_t *io,uint32_t pid)
{
    for(unsigned i=0;i<16;++i) if(io->nodes[i].used && io->nodes[i].pid==pid) return &io->nodes[i];
    return NULL;
}
static int16_t pcm(uint32_t bits)
{
    unsigned exponent=(bits>>23)&255u;
    if(!exponent || exponent==255u) return 0;
    unsigned magnitude;
    if(exponent>127) magnitude=32768;
    else {
        unsigned shift=135u-exponent;
        magnitude=shift>=24 ? 0 : ((bits&0x7fffffu)|0x800000u)>>shift;
    }
    if(bits>>31) return (int16_t)-(int)(magnitude>32768?32768:magnitude);
    return (int16_t)(magnitude>32767?32767:magnitude);
}
static void publish(void *ctx,uint32_t pid,enum tc_output action)
{
    io_node_t *n=ctx; io_context_t *io=n->owner;
    ++n->calls;
    if(action!=TC_OUTPUT_OK) ++n->silent;
    if(pid==io->half_check && action==TC_OUTPUT_OK) {
        for(unsigned j=0;j<SAMPLES;++j) {
            uint32_t expected=(n->in[j]&0x7fffffffu)?n->in[j]-(1u<<23):n->in[j];
            if(n->out[j]!=expected) ++n->invalid;
        }
    }
    audio_graph_t *g=&io->manager->gc->graph;
    /* Fixture transport: one same-frame stereo producer per input. */
    for(unsigned e=0;e<GRAPH_MAX_EDGES;++e) {
        graph_edge_t *edge=&g->edges[e];
        if(!edge->used || g->nodes[edge->src].pid!=pid) continue;
        io_node_t *dest=io_find(io,g->nodes[edge->dst].pid);
        if(dest) for(unsigned j=0;j<SAMPLES;++j) dest->in[j]=n->out[j];
    }
    if(pid==io->sink) {
        int16_t output[SAMPLES];
        for(unsigned j=0;j<FRAMES;++j) { output[2*j]=pcm(n->out[j]); output[2*j+1]=pcm(n->out[j+FRAMES]); }
        if(spsc_write(&dac,output,SAMPLES)!=SAMPLES) ++n->invalid;
    }
}
static int make_binding(void *ctx,plugin_t *pl,tc_plugin_binding_t *binding)
{
    io_context_t *io=ctx; io_node_t *n=NULL;
    for(unsigned i=0;i<16;++i) if(!io->nodes[i].used) { n=&io->nodes[i];break; }
    if(!n) return -1;
    *n=(io_node_t){.owner=io,.used=1,.pid=pl->proc->pid};
    binding->ctx=n;
    uintptr_t in=phys_alloc_page_zero();
    if(!in) return -1;
    if(plugin_map_region(pl,RING_IN_VA,in,PAGE_SIZE,VMM_READ)) { phys_free_page(in);return -1; }
    uintptr_t out=phys_alloc_page_zero();
    if(!out) return -1;
    if(plugin_map_region(pl,RESULTS_VA,out,PAGE_SIZE,VMM_READ|VMM_WRITE)) { phys_free_page(out);return -1; }
    n->in=(uint32_t *)P2V(in);n->out=(uint32_t *)P2V(out);
    for(unsigned j=0;j<SAMPLES;++j) n->in[j]=0x3f800000u;
    binding->call=(budget_plugin_call_t){pl,RING_IN_VA,RING_IN_VA+FRAMES*4,
        RESULTS_VA,RESULTS_VA+FRAMES*4,n->out,n->out+FRAMES,FRAMES};
    binding->input_left=n->in;binding->input_right=n->in+FRAMES;binding->publish=publish;
    return 0;
}
static void release_binding(void *ctx,tc_plugin_binding_t *binding)
{ (void)ctx; if(binding->ctx) ((io_node_t *)binding->ctx)->used=0; }
static void register_sources(plugin_mgr_t *m)
{
    check(pm_register_blob(m,"good",rt_good_start,(size_t)(rt_good_end-rt_good_start))==PM_OK,"register good");
    check(pm_register_blob(m,"hog",rt_hog_start,(size_t)(rt_hog_end-rt_hog_start))==PM_OK,"register hog");
    for(unsigned i=0;i<5;++i)
        check(pm_register_blob(m,life_name[i],life_start[i],(size_t)((char *)life_end[i]-(char *)life_start[i]))==PM_OK,"register lifecycle fixture");
}
static temporal_contract_t contract(unsigned criticality,unsigned policy,uint64_t budget,uint64_t deadline)
{
    temporal_contract_t t={0}; t.period=period;t.deadline=deadline;t.cpu_budget=budget;
    t.criticality=criticality;t.overrun_policy=policy;t.kill_after=policy==TC_MUTE_THEN_KILL?3:0;return t;
}
void scheduler_tick(struct trapframe *tf)
{
    (void)tf;if(audio.serviced>=target) return;
    uint64_t release=timer_deadline()-timer_interval();
    ++sequence;
    aw_kick_at(&worker1,sequence,release);aw_kick_at(&worker3,sequence,release);
    uint64_t start=clock_now();
    if(audio_core_fill(&audio)<SAMPLES) ++underruns;
    audio_wd_account(&audio.wd,clock_now()-start);++audio.serviced;
}
static void worker_entry(void *arg)
{ mmu_join();exceptions_init();gic_cpu_init();aw_worker_loop(arg); }
/* This runs inside the live control client's SVC. */
long sys_plugin_load(const char *path)
{
    if(!path || path[0]!='h' || current_process()!=control.proc) return -100;
    size_t before=pmm_free_pages();
    long result=pm_load(&probe_manager,"hang-abi");
    if(result!=PM_ETIMEOUT || pmm_free_pages()!=before || current_process()!=control.proc) return -101;
    ++nested_count;
    return result;
}
static int lifecycle_probes(void)
{
    for(unsigned stage=0;stage<5;++stage) {
        size_t before=pmm_free_pages();
        long pid=pm_load(&probe_manager,life_name[stage]);
        if(stage==1) { if(pid!=PM_ETIMEOUT || pmm_free_pages()!=before) return 0; continue; }
        if(pid<=0) return 0;
        plugin_t *p=pm_plugin(&probe_manager,(uint32_t)pid);
        long result=plugin_call_init(p,48000,FRAMES);
        if(stage==2) { if(result!=PLUGIN_ETIMEOUT) return 0; }
        else {
            if(result!=0) return 0;
            result=plugin_call_set_param(p,7,0x3e800000u);
            if(stage==3) { if(result!=PLUGIN_ETIMEOUT) return 0; }
            else {
                if(result!=0) return 0;
                if(stage==0) {
                    io_context_t temp={0};tc_plugin_binding_t binding={0};binding.call.plugin=p;
                    if(make_binding(&temp,p,&binding)) return 0;
                    uint64_t elapsed;
                    if(budget_plugin_invoke(&binding.call,period/5,clock_now()+period,&elapsed)!=0) return 0;
                    for(unsigned j=0;j<SAMPLES;++j) if(((io_node_t *)binding.ctx)->out[j]!=0x3e800000u) return 0;
                }
                result=plugin_call_destroy(p);
                if(stage==4 ? result!=PLUGIN_ETIMEOUT : result!=0) return 0;
            }
        }
        if(pm_unload(&probe_manager,(uint32_t)pid)!=PM_OK || pmm_free_pages()!=before) return 0;
    }
    return 1;
}
static void control_entry(void *arg)
{
    (void)arg;mmu_join();exceptions_init();gic_cpu_init();
    while(!__atomic_load_n(&worker1.online,__ATOMIC_ACQUIRE) ||
          !__atomic_load_n(&worker3.online,__ATOMIC_ACQUIRE)) { }
    long result=plugin_call_init(&control,48000,FRAMES);
    control_ok=result==0 && control_result[8]==16 && !control_result[10] &&
        (long)control_result[11]==PM_ETIMEOUT && control_result[12]==1 && nested_count==1;
    probe_ok=lifecycle_probes();
    __atomic_store_n(&control_done,1u,__ATOMIC_RELEASE);
}
static void run_phase(unsigned blocks)
{
    target=blocks;audio.serviced=0;
    timer_init(HZ);__asm__ volatile("msr daifclr,#2");
    if (!cadence_probe_done) {
        uint64_t before_ticks=timer_ticks();
        long result=plugin_call_init(&cadence_probe,48000,FRAMES);
        check(result==PLUGIN_ETIMEOUT && timer_ticks()>=before_ticks+2,
              "physical cadence continues during same-core virtual budget timeout");
        cadence_probe_done=1;
    }
    while(audio.serviced<target) __asm__ volatile("wfi");
    timer_stop();__asm__ volatile("msr daifset,#2");
    check(tr_pause(&runtime1,frequency)==TC_OK,"pause primary worker");
    check(tr_pause(&runtime3,frequency)==TC_OK,"pause secondary worker");
}
void test_main(void)
{
    uart_virt_init();uart_puts("\r\n=== managed temporal runtime / four cores ===\r\n");
    pmm_init();mmu_init();exceptions_init();gic_init();
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));period=frequency/HZ;
    gc_ring_ops_t graph_ops={ring_new,ring_free,ring_map,ring_unmap,NULL};
    gc_init(&graph1,&graph_ops);gc_init(&graph3,&graph_ops);
    pm_init(&manager1,&graph1);pm_init(&manager3,&graph3);pm_init(&probe_manager,NULL);
    register_sources(&manager1);register_sources(&manager3);register_sources(&probe_manager);
    pm_set_lifecycle_budget(&probe_manager,frequency/500u);
    size_t baseline=pmm_free_pages();
    aw_init(&worker1,1);aw_init(&worker3,3);
    io1.manager=&manager1;io3.manager=&manager3;
    tr_io_ops_t first_io={make_binding,release_binding,&io1},other_io={make_binding,release_binding,&io3};
    tc_limits_t limits={period,period/100u,period/40u};
    check(tr_init(&runtime1,&manager1,&worker1,&limits,clock_now,48000,FRAMES,frequency/500u,&first_io)==TC_OK,"first managed runtime");
    check(tr_init(&runtime3,&manager3,&worker3,&limits,clock_now,48000,FRAMES,frequency/500u,&other_io)==TC_OK,"second managed runtime");
    temporal_contract_t good=contract(TC_HARD,TC_MUTE,period/6,period/2);
    temporal_contract_t gain=contract(TC_HARD,TC_MUTE,period/6,period*3/4);
    temporal_contract_t hog=contract(TC_SOFT,TC_MUTE_THEN_KILL,period/20,period*9/10);
    long first=tr_load(&runtime1,"good",&good),second=tr_load(&runtime1,"gain",&gain);
    long bad1=tr_load(&runtime1,"hog",&hog),other=tr_load(&runtime3,"good",&good),bad3=tr_load(&runtime3,"hog",&hog);
    check(first>0 && second>0 && bad1>0 && other>0 && bad3>0,"managed load and finite initialization");
    check(tr_connect(&runtime1,(uint32_t)first,(uint32_t)second,0)==TC_OK,"admitted real signal chain");
    io1.sink=(uint32_t)second;io1.half_check=(uint32_t)second;
    size_t before=pmm_free_pages();
    temporal_contract_t impossible=gain;impossible.cpu_budget=period;
    check(tr_load(&runtime1,"gain",&impossible)==TC_EINVAL && pmm_free_pages()==before,"failed admission rolls load back");
    check(tr_load(&runtime1,"hang-abi",&gain)==TR_EPLUGIN && pmm_free_pages()==before,"managed ABI timeout cleanup");
    check(tr_load(&runtime1,"hang-init",&gain)==TR_EPLUGIN && pmm_free_pages()==before,"managed initialization timeout cleanup");
    check(pm_unload(&manager1,(uint32_t)first)==PM_EBUSY && pm_load(&manager1,"good")==PM_EBUSY &&
          gc_disconnect(&graph1,(uint32_t)first,(uint32_t)second)==GC_EBUSY,"raw lifecycle/graph bypass refused");
    check(plugin_load(&control,rt_ctl_start,(size_t)(rt_ctl_end-rt_ctl_start),"live-control")==PLUGIN_OK,"load trusted control client");
    control.lifecycle_ticks=frequency;
    check(plugin_load(&cadence_probe,rt_life2_start,(size_t)(rt_life2_end-rt_life2_start),
          "cadence-probe")==PLUGIN_OK,"load same-core timer probe");
    process_set_svc_gate(cadence_probe.proc,PLUGIN_TRAMP_VA);
    cadence_probe.lifecycle_ticks=period*3u;
    uintptr_t page=phys_alloc_page_zero();
    check(page && !plugin_map_region(&control,RESULTS_VA,page,PAGE_SIZE,VMM_READ|VMM_WRITE),"control results mapping");
    control_result=(volatile uint64_t *)P2V(page);
    control_result[0]=(uint32_t)first;control_result[1]=period;
    control_result[2]=good.deadline;control_result[3]=good.cpu_budget;
    tc_host_bind_control(&runtime1.host);
    spsc_init(&dac,dac_storage,4096);audio_core_init(&audio,&dac,dma,FRAMES,period/2);
    int16_t silence[SAMPLES]={0};spsc_write(&dac,silence,SAMPLES);spsc_write(&dac,silence,SAMPLES);
    check(tr_start(&runtime1)==TC_OK && tr_start(&runtime3)==TC_OK,"admit and start both cores");
    check(tr_unload(&runtime1,(uint32_t)first)==TC_EBUSY &&
        tr_connect(&runtime1,(uint32_t)first,(uint32_t)second,0)==TC_EBUSY,
        "running runtime refuses destructive edits without pause");
    check(!smp_start_core(1,worker_entry,&worker1,(uint64_t)(uintptr_t)(stacks[0]+sizeof stacks[0])),"boot DSP core one");
    check(!smp_start_core(3,worker_entry,&worker3,(uint64_t)(uintptr_t)(stacks[2]+sizeof stacks[2])),"boot DSP core three");
    check(!smp_start_core(2,control_entry,NULL,(uint64_t)(uintptr_t)(stacks[1]+sizeof stacks[1])),"boot live control core");
    while(!__atomic_load_n(&worker1.online,__ATOMIC_ACQUIRE) || !__atomic_load_n(&worker3.online,__ATOMIC_ACQUIRE)) { }
    run_phase(128);
    uint64_t wait=clock_now();
    while(!__atomic_load_n(&control_done,__ATOMIC_ACQUIRE) && clock_now()-wait<frequency) { }
    check(control_done && control_ok && probe_ok,"live SVC updates, nested timeout, FP and all lifecycle probes");
    const tc_task_state_t *a=tc_state(&runtime1.host.scheduler,(uint32_t)first);
    const tc_task_state_t *b=tc_state(&runtime1.host.scheduler,(uint32_t)second);
    const tc_task_state_t *c=tc_state(&runtime3.host.scheduler,(uint32_t)other);
    check(a && b && c && a->completed==128 && b->completed==128 && c->completed==128 &&
          !a->deadline_misses && !b->deadline_misses && !c->deadline_misses,"three hard jobs survive concurrent hostile callbacks");
    check(!underruns && !audio.wd.overruns && !worker1.overruns && !worker3.overruns,"zero audio misses across four cores");
    check(!io_find(&io1,(uint32_t)second)->invalid,"actual producer-to-gain output bit exact");
    check(tc_state(&runtime1.host.scheduler,(uint32_t)bad1)->killed &&
          tc_state(&runtime3.host.scheduler,(uint32_t)bad3)->killed,"independent budget termination on both worker cores");
    uart_printf("concurrent: 128 frames; 3 HARD jobs/frame; live SVC updates=%u; nested ABI timeout=%u; FP preserved=%u\r\n",
        (unsigned)control_result[8],nested_count,(unsigned)control_result[12]);
    check(tr_connect(&runtime1,(uint32_t)second,(uint32_t)first,0)==TC_EDEPENDENCY,"bad rewire rejected before mutation");
    check(tr_disconnect(&runtime1,(uint32_t)first,(uint32_t)second)==TC_OK &&
          tr_connect(&runtime1,(uint32_t)first,(uint32_t)second,0)==TC_OK,"managed pause rewire and readmission");
    temporal_contract_t paused_edit=good;paused_edit.pid=(uint32_t)first;
    check(tr_set_contract(&runtime1,&paused_edit)==TC_OK && !runtime1.host.scheduler.pending_ready,
          "paused contract edit permits following lifecycle operations");
    before=pmm_free_pages();
    for(unsigned i=0;i<100;++i) {
        long p=tr_load(&runtime1,"gain",&hog);
        check(p>0 && tr_unload(&runtime1,(uint32_t)p)==TC_OK && pmm_free_pages()==before,"100 managed load/unload cycles");
    }
    long doomed=tr_load(&runtime1,"hang-destroy",&hog);
    check(doomed>0 && tr_start(&runtime1)==TC_OK,"restart existing worker after graph edit");
    run_phase(32);
    check(tc_state(&runtime1.host.scheduler,(uint32_t)first)->completed==160 &&
        !tc_state(&runtime1.host.scheduler,(uint32_t)first)->missed_releases,"intentional pause resets epoch without false misses");
    check(tr_unload(&runtime1,(uint32_t)doomed)==TR_EPLUGIN &&
        runtime1.last_lifecycle_result==PLUGIN_ETIMEOUT,"hung destructor bounded and reclaimed");
    check(tr_shutdown(&runtime1,frequency)==TC_OK && tr_shutdown(&runtime3,frequency)==TC_OK,"automatic shutdown/unbind/reclaim");
    aw_stop(&worker1);aw_stop(&worker3);
    process_destroy(control.proc);
    process_destroy(cadence_probe.proc);
    check(!ring_live && pmm_free_pages()==baseline,"all graph rings, process frames and aliases reclaimed");
    check(sys_plugin_set_contract((uint32_t)first,period,good.deadline,good.cpu_budget,0,0)==TC_ENODEV,"shutdown clears control binding");
    uart_printf("lifecycle: ABI/init/parameter/destroy timeout containment PASS; load/unload=100; resume=32 frames\r\n");
    uart_printf("audio: underruns=%u watchdog=%u worker1_skips=%u worker3_skips=%u\r\n",
        (unsigned)underruns,(unsigned)audio.wd.overruns,(unsigned)worker1.overruns,(unsigned)worker3.overruns);
    uart_puts("same-core cadence and budget timers: PASS\r\n");
    uart_puts("RUNTIME: PASS\r\n");m12_finish();
}
