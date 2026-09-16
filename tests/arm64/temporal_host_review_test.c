/* Host adapter lifecycle/control regressions; real EL0 execution is covered
 * separately by temporal_acceptance_main.c. */
#include "temporal_host.h"
#include "usermode.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

static uint64_t now;
static unsigned checks, publications;
#define CHECK(x) do { checks++; if(!(x)) { \
 fprintf(stderr,"HOST REVIEW FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static uint64_t clock_fn(void) { return now; }
long budget_plugin_invoke(budget_plugin_call_t *c, uint64_t budget,
                          uint64_t cutoff, uint64_t *elapsed)
{
    if(c->plugin->proc->state==PROC_KILLED) return -1;
    if(now>=cutoff || cutoff-now<5) return BUDGET_DEADLINE;
    if(budget<5) return BUDGET_PREEMPTED;
    now+=5;
    if(elapsed) *elapsed=5;
    uint32_t *l=c->left,*r=c->right;
    for(uint32_t i=0;i<c->frames;i++) l[i]=r[i]=0x3f000000;
    return 0;
}
void process_kill(process_t *p, long code)
{ p->state=PROC_KILLED; p->exit_code=code; }
static void publish(void *ctx, uint32_t pid, enum tc_output action)
{ (void)ctx; (void)pid; (void)action; publications++; }
int main(void)
{
    graph_control_t gc; gc_ring_ops_t rops={0}; gc_init(&gc,&rops);
    tc_limits_t limits={1000,20,10}; temporal_host_t host;
    tc_host_init(&host,&gc,&limits,clock_fn);
    audio_worker_t w; aw_init(&w,1);
    process_t proc={0}; proc.pid=1;
    plugin_t plugin={.proc=&proc}; uint32_t left[8]={0},right[8]={0},input[8]={0};
    tc_plugin_binding_t b={.call={.plugin=&plugin,.left=left,.right=right,.frames=8},
        .input_left=input,.input_right=input,.publish=publish};
    CHECK(gc_add_plugin(&gc,1)>=0);
    CHECK(tc_host_bind(&host,&b)==TC_EINVAL);
    proc.svc_gate=PLUGIN_TRAMP_VA;
    CHECK(tc_host_bind(&host,&b)==TC_OK);
    CHECK(tc_host_bind(&host,&b)==TC_EINVAL);
    temporal_contract_t c={.pid=1,.criticality=TC_HARD,.period=1000,
        .deadline=900,.cpu_budget=100,.overrun_policy=TC_MUTE};
    CHECK(tc_host_stage(&host,&c,1)==TC_OK);
    CHECK(tc_host_unbind(&host,1)==TC_EBUSY);
    CHECK(tc_host_attach(&host,&w)==TC_OK);
    CHECK(tc_host_attach(&host,&w)==TC_EINVAL);
    CHECK(aw_kick_at(&w,1,1000)==1); now=1020;
    CHECK(aw_worker_step(&w)==1 && host.last_result==TC_OK);
    CHECK(tc_state(&host.scheduler,1)->completed==1);
    CHECK(aw_kick_at(&w,2,2000)==1);
    CHECK(tc_host_unbind(&host,1)==TC_EBUSY);
    now=2020; CHECK(aw_worker_step(&w)==1);
    tc_host_bind_control(&host);
    CHECK(sys_plugin_set_contract(1,1000,900,100,UINT64_MAX,0)==TC_EINVAL);
    CHECK(sys_plugin_set_contract(1,1000,900,UINT64_MAX,0,0)==TC_EINVAL);
    CHECK(sys_plugin_set_contract(99,1000,900,100,0,0)==TC_ENODEV);
    CHECK(host.desired[0].cpu_budget==100 && !host.scheduler.pending_ready);
    CHECK(tc_host_unbind(&host,1)==TC_OK);
    CHECK(host.bindings==0 && host.desired_count==0 && !host.scheduler.active_valid);
    CHECK(tc_host_unbind(&host,1)==TC_ENODEV);
    audio_graph_remove_node(&gc.graph,audio_graph_node_by_pid(&gc.graph,1));
    for(unsigned i=0;i<100;i++) {
        proc=(process_t){0}; proc.pid=i+2; proc.svc_gate=PLUGIN_TRAMP_VA; c.pid=proc.pid;
        CHECK(gc_add_plugin(&gc,proc.pid)>=0);
        CHECK(tc_host_bind(&host,&b)==TC_OK);
        CHECK(tc_host_stage(&host,&c,1)==TC_OK);
        uint64_t seq=i+3;
        CHECK(aw_kick_at(&w,seq,seq*1000)==1); now=seq*1000+20;
        CHECK(aw_worker_step(&w)==1 && host.last_result==TC_OK);
        CHECK(tc_state(&host.scheduler,proc.pid)->completed==1);
        CHECK(tc_host_unbind(&host,proc.pid)==TC_OK);
        CHECK(host.bindings==0 && host.desired_count==0);
        audio_graph_remove_node(&gc.graph,audio_graph_node_by_pid(&gc.graph,proc.pid));
    }
    /* No active plan and no bindings: a stray kick cannot use reclaimed I/O. */
    unsigned before=publications;
    CHECK(aw_kick_at(&w,103,103000)==1); now=103020;
    CHECK(aw_worker_step(&w)==1 && host.last_result==TC_ENODEV);
    CHECK(publications==before);
    tc_host_bind_control(NULL);
    CHECK(sys_plugin_set_contract(1,1000,900,100,0,0)==TC_ENODEV);
    printf("TEMPORAL ADAPTER REVIEW: PASS (%u assertions; 100 bind/run/unbind cycles)\n",checks);
    return 0;
}
