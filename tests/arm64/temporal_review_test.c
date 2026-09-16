/* Independent adversarial regression tests for the temporal contract layer. */
#include "temporal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
 fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

typedef struct {
    uint64_t now, overhead, cost[TC_MAX_TASKS], limit[TC_MAX_TASKS];
    uint32_t calls[TC_MAX_TASKS], kills[TC_MAX_TASKS], output[TC_MAX_TASKS];
    uint32_t order[1024], order_n;
    long result[TC_MAX_TASKS];
} fake_t;
static uint64_t clock_fn(void *arg) { return ((fake_t *)arg)->now; }
static long run_fn(void *arg, uint32_t pid, uint64_t budget, uint64_t cutoff)
{
    fake_t *f = arg; unsigned i = pid - 1;
    assert(i < TC_MAX_TASKS && f->now <= cutoff);
    f->calls[i]++; f->limit[i] = budget;
    if (f->order_n < 1024) f->order[f->order_n++] = pid;
    if (f->result[i] == TC_RUN_BUDGET) f->now += budget;
    else if (f->result[i] == TC_RUN_DEADLINE) f->now = cutoff;
    else f->now += f->cost[i] ? f->cost[i] : budget - 1;
    return f->result[i];
}
static void output_fn(void *arg, uint32_t pid, enum tc_output out)
{
    fake_t *f = arg; f->output[pid - 1] = (uint32_t)out;
    f->now += f->overhead;
}
static void kill_fn(void *arg, uint32_t pid) { ((fake_t *)arg)->kills[pid - 1]++; }
static tc_ops_t ops(fake_t *f)
{ return (tc_ops_t){clock_fn, run_fn, output_fn, kill_fn, f}; }
static audio_graph_t graph(unsigned n)
{
    audio_graph_t g; audio_graph_init(&g, NULL);
    for (unsigned i = 1; i <= n; i++) assert(audio_graph_add_node(&g, i) >= 0);
    return g;
}
static temporal_contract_t contract(unsigned pid, uint64_t p, uint64_t d, uint64_t c)
{
    return (temporal_contract_t){ .pid=pid, .period=p, .deadline=d, .cpu_budget=c,
        .criticality=TC_HARD, .overrun_policy=TC_MUTE };
}
static int admission(void)
{
    audio_graph_t g = graph(2); tc_limits_t l = {1000, 20, 10};
    temporal_contract_t c[2] = {contract(1,1000,900,100),contract(2,1000,500,100)};
    tc_plan_t p, old; tc_admission_t why;
    CHECK(tc_plan_build(&g,c,2,&l,&p,&why)==TC_OK);
    CHECK(p.order[0]==1 && p.order[1]==0 && p.reserved_ticks==240);
    old=p;
    c[1].deadline=125; c[1].cpu_budget=110;
    CHECK(tc_plan_build(&g,c,2,&l,&p,&why)==TC_EADMISSION);
    CHECK(why.pid==2 && why.required==140 && why.available==125);
    CHECK(memcmp(&old,&p,sizeof p)==0);
    c[1]=contract(2,1000,500,100); c[0].period=1500;
    CHECK(tc_plan_build(&g,c,2,&l,&p,&why)==TC_EINVAL);
    c[0].period=UINT64_MAX;
    CHECK(tc_plan_build(&g,c,2,&l,&p,&why)==TC_EINVAL);
    c[0]=contract(1,1000,900,100); c[1].pid=1;
    CHECK(tc_plan_build(&g,c,2,&l,&p,&why)==TC_EINVAL);
    c[1].pid=7;
    CHECK(tc_plan_build(&g,c,2,&l,&p,&why)==TC_ENODEV);
    c[1].pid=2; c[1].criticality=UINT32_MAX;
    CHECK(tc_plan_build(&g,c,2,&l,&p,&why)==TC_EINVAL);
    return 0;
}
static int dependencies(void)
{
    audio_graph_t g=graph(2); tc_limits_t l={1000,20,10}; tc_plan_t p;
    temporal_contract_t c[2]={contract(1,1000,900,100),contract(2,1000,500,100)};
    CHECK(audio_graph_connect(&g,0,1)>=0);
    CHECK(tc_plan_build(&g,c,2,&l,&p,NULL)==TC_OK);
    CHECK(p.order[0]==0 && p.order[1]==1);
    c[0].criticality=TC_SOFT;
    CHECK(tc_plan_build(&g,c,2,&l,&p,NULL)==TC_EDEPENDENCY);
    c[0].criticality=TC_HARD; c[0].period=2000;
    CHECK(tc_plan_build(&g,c,2,&l,&p,NULL)==TC_EDEPENDENCY);
    c[1].period=4000;
    CHECK(tc_plan_build(&g,c,2,&l,&p,NULL)==TC_OK);
    int dac=audio_graph_add_dac(&g); CHECK(dac>=0);
    CHECK(audio_graph_connect(&g,1,dac)>=0);
    CHECK(tc_plan_build(&g,c,2,&l,&p,NULL)==TC_EDEPENDENCY);
    return 0;
}
static int periods_and_updates(void)
{
    audio_graph_t g=graph(2); tc_limits_t l={1000,20,10};
    temporal_contract_t c[2]={contract(1,1000,900,100),contract(2,2000,500,100)};
    temporal_scheduler_t s; tc_scheduler_init(&s); fake_t f={.overhead=10}; tc_ops_t o=ops(&f);
    CHECK(tc_stage(&s,&g,c,2,&l,NULL)==TC_OK);
    CHECK(tc_stage(&s,&g,c,2,&l,NULL)==TC_EBUSY);
    for (uint64_t frame=1;frame<=8;frame++) {
        f.now=frame*1000+20;
        CHECK(tc_run_frame(&s,frame,frame*1000,&o)==TC_OK);
        CHECK(tc_run_frame(&s,frame,frame*1000,&o)==TC_ESTALE);
    }
    CHECK(f.calls[0]==8 && f.calls[1]==4);
    CHECK(tc_state(&s,1)->completed==8 && tc_state(&s,2)->completed==4);
    CHECK(tc_state(&s,1)->deadline_misses==0 && tc_state(&s,2)->deadline_misses==0);
    c[0].cpu_budget=120;
    CHECK(tc_stage(&s,&g,c,2,&l,NULL)==TC_OK);
    CHECK(tc_run_frame(&s,8,8000,&o)==TC_ESTALE && s.pending_ready);
    f.now=12020;
    CHECK(tc_run_frame(&s,12,12000,&o)==TC_OK && !s.pending_ready);
    CHECK(tc_state(&s,1)->missed_releases==3);
    CHECK(tc_state(&s,2)->missed_releases==2);
    CHECK(f.calls[0]==9 && f.calls[1]==4 && f.limit[0]==120);
    l.frame_ticks=2000;
    CHECK(tc_stage(&s,&g,c,2,&l,NULL)==TC_EINVAL);
    return 0;
}
static int policies(void)
{
    audio_graph_t g=graph(5); tc_limits_t l={1000,20,10};
    temporal_contract_t c[5];
    for(unsigned i=0;i<5;i++) c[i]=contract(i+1,1000,1000,100);
    c[0].overrun_policy=TC_MUTE;
    c[1].overrun_policy=TC_BYPASS;
    c[2].overrun_policy=TC_KILL;
    c[3].overrun_policy=TC_MUTE_THEN_KILL; c[3].kill_after=2;
    c[4].overrun_policy=TC_DEGRADE; c[4].criticality=TC_SOFT; c[4].skip_periods=2;
    temporal_scheduler_t s; tc_scheduler_init(&s); fake_t f={.overhead=10}; tc_ops_t o=ops(&f);
    for(unsigned i=0;i<5;i++) f.result[i]=TC_RUN_BUDGET;
    CHECK(tc_stage(&s,&g,c,5,&l,NULL)==TC_OK);
    for(uint64_t frame=1;frame<=4;frame++) {
        f.now=frame*1000+20;
        CHECK(tc_run_frame(&s,frame,frame*1000,&o)==TC_OK);
    }
    CHECK(f.calls[0]==4 && f.kills[0]==0 && f.output[0]==TC_OUTPUT_SILENCE);
    CHECK(f.calls[1]==4 && f.kills[1]==0 && f.output[1]==TC_OUTPUT_BYPASS);
    CHECK(f.calls[2]==1 && f.kills[2]==1 && f.output[2]==TC_OUTPUT_SILENCE);
    CHECK(f.calls[3]==2 && f.kills[3]==1);
    CHECK(f.calls[4]==2 && tc_state(&s,5)->shed==2);
    CHECK(tc_stage(&s,&g,c,5,&l,NULL)==TC_OK);
    f.now=5020; CHECK(tc_run_frame(&s,5,5000,&o)==TC_OK);
    CHECK(f.calls[2]==1 && f.calls[3]==2 && f.kills[2]==1 && f.kills[3]==1);
    return 0;
}
static int overload_and_deadlines(void)
{
    audio_graph_t g=graph(2); tc_limits_t l={1000,20,10};
    temporal_contract_t c[2]={contract(1,1000,800,700),contract(2,1000,1000,500)};
    c[1].criticality=TC_BEST_EFFORT;
    temporal_scheduler_t s; tc_scheduler_init(&s); fake_t f={.overhead=10}; tc_ops_t o=ops(&f);
    CHECK(tc_stage(&s,&g,c,2,&l,NULL)==TC_OK);
    f.now=1020; CHECK(tc_run_frame(&s,1,1000,&o)==TC_OK);
    CHECK(f.calls[0]==1 && f.calls[1]==0 && tc_state(&s,2)->shed==1);
    CHECK(tc_state(&s,1)->deadline_misses==0 && tc_state(&s,1)->completed==1);
    f.now=2500; CHECK(tc_run_frame(&s,2,2000,&o)==TC_OK);
    CHECK(f.calls[0]==1 && tc_state(&s,1)->deadline_misses==1);
    CHECK(tc_state(&s,1)->budget_overruns==0 && !f.kills[0]);
    return 0;
}

static int test_rate_update(void)
{
    audio_graph_t g=graph(1); tc_limits_t l={1000,20,10};
    temporal_contract_t c=contract(1,1000,900,100);
    temporal_scheduler_t s; tc_scheduler_init(&s);
    fake_t f={.overhead=10}; tc_ops_t o=ops(&f);
    CHECK(tc_stage(&s,&g,&c,1,&l,NULL)==TC_OK);
    f.now=1020; CHECK(tc_run_frame(&s,1,1000,&o)==TC_OK);
    c.period=2000;
    CHECK(tc_stage(&s,&g,&c,1,&l,NULL)==TC_OK);
    f.now=4020; CHECK(tc_run_frame(&s,4,4000,&o)==TC_OK);
    CHECK(tc_state(&s,1)->missed_releases==2);
    CHECK(tc_state(&s,1)->deadline_misses==2);
    CHECK(tc_state(&s,1)->releases==3);
    return 0;
}

static uint32_t rng=0x53123;
static uint32_t next(void) { rng=rng*1664525u+1013904223u; return rng; }
static int admitted_schedules(void)
{
    for(unsigned trial=0;trial<1500;trial++) {
        unsigned n=1+next()%8; audio_graph_t g=graph(n);
        tc_limits_t l={1000,20,10}; temporal_contract_t c[TC_MAX_TASKS];
        for(unsigned i=0;i<n;i++) {
            c[i]=contract(i+1,(1+next()%4)*1000,100+next()%901,1+next()%200);
            c[i].criticality=next()%3;
        }
        temporal_scheduler_t s; tc_scheduler_init(&s); fake_t f={.overhead=10}; tc_ops_t o=ops(&f);
        if(tc_stage(&s,&g,c,n,&l,NULL)!=TC_OK) continue;
        for(uint64_t frame=1;frame<=13;frame++) {
            f.now=frame*1000+20;
            CHECK(tc_run_frame(&s,frame,frame*1000,&o)==TC_OK);
            CHECK(f.now<=frame*1000+1000);
            for(unsigned i=0;i<n;i++) if(c[i].criticality!=TC_BEST_EFFORT) {
                CHECK(tc_state(&s,i+1)->deadline_misses==0);
                CHECK(tc_state(&s,i+1)->shed==0);
                CHECK(tc_state(&s,i+1)->budget_overruns==0);
            }
        }
    }
    return 0;
}
int main(void)
{
    if(admission()||dependencies()||periods_and_updates()||policies()||
       overload_and_deadlines()||test_rate_update()||admitted_schedules()) return 1;
    printf("TEMPORAL INDEPENDENT REVIEW: PASS (%u assertions)\n",checks);
    return 0;
}
