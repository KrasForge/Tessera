/* Deterministic temporal-contract tests: no wall-clock tolerance or sleeping. */
#include "temporal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <threads.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static const tc_limits_t limits = {1000, 10, 10};
static temporal_contract_t task(uint32_t pid, uint64_t period, uint64_t deadline,
                                 uint64_t budget, unsigned crit, unsigned policy)
{
    temporal_contract_t c = {0};
    c.pid = pid; c.period = period; c.deadline = deadline; c.cpu_budget = budget;
    c.criticality = crit; c.overrun_policy = policy;
    c.kill_after = policy == TC_MUTE_THEN_KILL ? 3 : 0;
    c.skip_periods = policy == TC_DEGRADE ? 2 : 0;
    return c;
}
static void graph(audio_graph_t *g, unsigned n)
{
    audio_graph_init(g, NULL);
    for (unsigned i = 1; i <= n; ++i) CHECK(audio_graph_add_node(g, i) >= 0);
}

typedef struct {
    uint64_t now, cost[17], runs[17], kills[17], silenced[17], bypassed[17];
    long result[17];
    unsigned order[64], count;
    uint64_t last_cutoff, output_cost;
    unsigned output[17];
} fake_t;
static uint64_t clock_fn(void *ctx) { return ((fake_t *)ctx)->now; }
static long run_fn(void *ctx, uint32_t pid, uint64_t budget, uint64_t cutoff)
{
    fake_t *f = ctx;
    if (f->count < 64) f->order[f->count++] = pid;
    ++f->runs[pid];
    f->output[pid] = 0xdead; /* intentionally dirty before an injected hang */
    f->last_cutoff = cutoff;
    if (f->result[pid] == TC_RUN_BUDGET) f->now += budget;
    else f->now += f->cost[pid] ? f->cost[pid] : 20;
    return f->result[pid];
}
static void output_fn(void *ctx, uint32_t pid, enum tc_output action)
{
    fake_t *f = ctx;
    if (action == TC_OUTPUT_SILENCE) { ++f->silenced[pid]; f->output[pid] = 0; }
    if (action == TC_OUTPUT_BYPASS) { ++f->bypassed[pid]; f->output[pid] = 42; }
    f->now += f->output_cost;
}
static void kill_fn(void *ctx, uint32_t pid) { ++((fake_t *)ctx)->kills[pid]; }
static tc_ops_t ops_for(fake_t *f)
{ return (tc_ops_t){clock_fn, run_fn, output_fn, kill_fn, f}; }
static int frame(temporal_scheduler_t *s, fake_t *f, uint64_t seq, uint64_t lateness)
{
    f->now = seq * 1000 + lateness;
    tc_ops_t ops = ops_for(f);
    return tc_run_frame(s, seq, seq * 1000, &ops);
}

static void test_admission(void)
{
    audio_graph_t g; graph(&g, 2);
    temporal_contract_t c[2] = {task(1, 1000, 400, 100, TC_HARD, TC_MUTE),
                                task(2, 1000, 800, 200, TC_SOFT, TC_BYPASS)};
    tc_plan_t p, before;
    tc_admission_t why;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, &why) == TC_OK);
    CHECK(p.reserved_ticks == 330 && p.order[0] == 0 && p.order[1] == 1);
    before = p;
    c[1].deadline = 250;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, &why) == TC_EADMISSION);
    CHECK(why.pid == 2 && why.required == 330 && why.available == 250);
    CHECK(memcmp(&p, &before, sizeof p) == 0);
    c[1].period = 100000; /* tiny utilization still fails simultaneous short deadlines */
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, &why) == TC_EADMISSION);
    c[1].deadline = 800;
    c[1].period = 1500;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_EINVAL);
    c[1].period = UINT64_MAX;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_EINVAL);
    c[1].period = 1000; c[1].cpu_budget = 0;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_EINVAL);
    c[1].cpu_budget = 200; c[1].pid = 1;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_EINVAL);
    c[1].pid = 3;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_ENODEV);
    CHECK(tc_plan_build(&g, c, 1, &limits, &p, NULL) == TC_ENODEV);
    CHECK(tc_plan_build(NULL, c, 2, &limits, &p, NULL) == TC_EINVAL);
    tc_limits_t bad = limits; bad.job_overhead = UINT64_MAX;
    CHECK(tc_plan_build(&g, c, 2, &bad, &p, NULL) == TC_EINVAL);
    c[1] = task(2, 1000, 800, 200, TC_HARD, TC_DEGRADE);
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_EINVAL);
    c[1] = task(2, 1000, 800, 200, TC_SOFT, TC_MUTE_THEN_KILL); c[1].kill_after = 0;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_EINVAL);
}

static void test_dependencies(void)
{
    audio_graph_t g; graph(&g, 2);
    CHECK(audio_graph_connect(&g, 0, 1) >= 0);
    temporal_contract_t c[2] = {task(1, 1000, 800, 100, TC_HARD, TC_MUTE),
                                task(2, 2000, 400, 100, TC_HARD, TC_MUTE)};
    tc_plan_t p;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_OK);
    CHECK(p.order[0] == 0 && p.order[1] == 1); /* precedence before EDF */
    c[0].period = 4000;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_EDEPENDENCY);
    c[0].period = 1000; c[0].criticality = TC_SOFT;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_EDEPENDENCY);
    c[0].criticality = TC_HARD;
    int dac = audio_graph_add_dac(&g);
    CHECK(audio_graph_connect(&g, 1, dac) >= 0);
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_EDEPENDENCY);
    c[1].period = 1000;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_OK);
    CHECK(audio_graph_connect_feedback(&g, 1, 0) >= 0);
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_OK);
    c[0].period = c[1].period = 2000;
    CHECK(tc_plan_build(&g, c, 2, &limits, &p, NULL) == TC_EDEPENDENCY);
}

static void test_rates_and_edf(void)
{
    audio_graph_t g; graph(&g, 3);
    temporal_contract_t c[] = {task(1, 1000, 500, 100, TC_HARD, TC_MUTE),
        task(2, 2000, 300, 80, TC_HARD, TC_MUTE),
        task(3, 4000, 900, 100, TC_SOFT, TC_MUTE)};
    temporal_scheduler_t s; tc_scheduler_init(&s);
    CHECK(tc_stage(&s, &g, c, 3, &limits, NULL) == TC_OK);
    CHECK(tc_stage(&s, &g, c, 3, &limits, NULL) == TC_EBUSY);
    fake_t f = {0};
    for (unsigned b = 1; b <= 9; ++b) CHECK(frame(&s, &f, b, 10) == TC_OK);
    CHECK(f.order[0] == 2 && f.order[1] == 1 && f.order[2] == 3);
    CHECK(f.runs[1] == 9 && f.runs[2] == 5 && f.runs[3] == 3);
    CHECK(tc_state(&s, 1)->completed == 9 && tc_state(&s, 2)->deadline_misses == 0);
    CHECK(frame(&s, &f, 9, 10) == TC_ESTALE && f.runs[1] == 9);
    CHECK(frame(&s, &f, 8, 10) == TC_ESTALE);
    tc_ops_t ops = ops_for(&f); f.now = 10000;
    CHECK(tc_run_frame(&s, 10, 9999, &ops) == TC_EINVAL); /* cannot replenish early */
    CHECK(tc_run_frame(&s, 10, UINT64_MAX - 1, &ops) == TC_EINVAL);
}

static void test_policies(void)
{
    for (unsigned policy = TC_MUTE; policy <= TC_DEGRADE; ++policy) {
        audio_graph_t g; graph(&g, 2);
        temporal_contract_t c[] = {task(1, 1000, 400, 100, TC_HARD, TC_MUTE),
                                  task(2, 1000, 800, 100, TC_SOFT, policy)};
        temporal_scheduler_t s; tc_scheduler_init(&s);
        CHECK(tc_stage(&s, &g, c, 2, &limits, NULL) == TC_OK);
        fake_t f = {0}; f.result[2] = TC_RUN_BUDGET;
        for (unsigned b = 1; b <= 4; ++b) CHECK(frame(&s, &f, b, 10) == TC_OK);
        const tc_task_state_t *v = tc_state(&s, 2);
        CHECK(f.runs[1] == 4 && tc_state(&s, 1)->budget_overruns == 0);
        if (policy == TC_KILL) CHECK(v->killed && f.runs[2] == 1 && f.kills[2] == 1);
        else if (policy == TC_MUTE_THEN_KILL) CHECK(v->killed && f.runs[2] == 3 && f.kills[2] == 1);
        else if (policy == TC_DEGRADE) CHECK(!v->killed && f.runs[2] == 2 && v->shed == 2 && v->degraded == 2);
        else CHECK(!v->killed && f.runs[2] == 4 && v->budget_overruns == 4);
        CHECK(f.output[2] == (policy == TC_BYPASS ? 42u : 0u));
        CHECK(tc_stage(&s, &g, c, 2, &limits, NULL) == TC_OK);
        CHECK(frame(&s, &f, 5, 10) == TC_OK);
        if (policy == TC_KILL || policy == TC_MUTE_THEN_KILL)
            CHECK(f.kills[2] == 1 && tc_state(&s, 2)->killed); /* no reconfigure resurrection */
    }
}

static void test_lateness_and_recovery(void)
{
    audio_graph_t g; graph(&g, 2);
    temporal_contract_t c[] = {task(1, 1000, 400, 100, TC_HARD, TC_MUTE),
                              task(2, 2000, 800, 100, TC_SOFT, TC_MUTE_THEN_KILL)};
    temporal_scheduler_t s; tc_scheduler_init(&s);
    CHECK(tc_stage(&s, &g, c, 2, &limits, NULL) == TC_OK);
    fake_t f = {0}; f.result[2] = TC_RUN_BUDGET;
    CHECK(frame(&s, &f, 1, 10) == TC_OK);
    CHECK(frame(&s, &f, 3, 10) == TC_OK);
    CHECK(tc_state(&s, 1)->missed_releases == 1 && tc_state(&s, 2)->streak == 2);
    f.result[2] = 0;
    CHECK(frame(&s, &f, 5, 10) == TC_OK);
    CHECK(tc_state(&s, 2)->streak == 0 && !tc_state(&s, 2)->killed);
    CHECK(frame(&s, &f, 9, 600) == TC_OK);
    CHECK(tc_state(&s, 1)->missed_releases == 5);
    CHECK(tc_state(&s, 2)->missed_releases == 1);
    CHECK(tc_state(&s, 1)->budget_overruns == 0 && tc_state(&s, 1)->deadline_misses == 6);
    CHECK(f.kills[1] == 0); /* late dispatch is not a plugin budget offence */
    f.result[2] = TC_RUN_DEADLINE;
    CHECK(frame(&s, &f, 11, 10) == TC_OK);
    CHECK(tc_state(&s, 2)->deadline_misses == 2 && tc_state(&s, 2)->budget_overruns == 2);
    f.result[2] = -1;
    CHECK(frame(&s, &f, 13, 10) == TC_OK);
    CHECK(tc_state(&s, 2)->faults == 1 && f.kills[2] == 1 && f.output[2] == 0);
}

static void test_best_effort_and_updates(void)
{
    audio_graph_t g; graph(&g, 3);
    temporal_contract_t c[] = {task(1, 1000, 800, 600, TC_HARD, TC_MUTE),
        task(2, 1000, 900, 200, TC_SOFT, TC_MUTE),
        task(3, 1000, 1000, 800, TC_BEST_EFFORT, TC_BYPASS)};
    temporal_scheduler_t s; tc_scheduler_init(&s);
    CHECK(tc_stage(&s, &g, c, 3, &limits, NULL) == TC_OK);
    fake_t f = {0}; f.cost[1] = 500; f.cost[2] = 180;
    CHECK(frame(&s, &f, 1, 10) == TC_OK);
    CHECK(f.runs[1] == 1 && f.runs[2] == 1 && !f.runs[3] && f.output[3] == 42);
    CHECK(tc_state(&s, 3)->shed == 1 && tc_state(&s, 1)->deadline_misses == 0);
    c[1].cpu_budget = 400;
    CHECK(tc_stage(&s, &g, c, 3, &limits, NULL) == TC_EADMISSION);
    CHECK(!s.pending_ready && s.active.task[1].cpu_budget == 200);
    c[1].cpu_budget = 150;
    CHECK(tc_stage(&s, &g, c, 3, &limits, NULL) == TC_OK);
    f.cost[1] = f.cost[2] = 20;
    CHECK(frame(&s, &f, 2, 10) == TC_OK);
    CHECK(s.active.task[1].cpu_budget == 150 && f.runs[3] == 1);
    tc_limits_t changed = limits; changed.frame_ticks = 2000;
    CHECK(tc_stage(&s, &g, c, 3, &changed, NULL) == TC_EINVAL);
}

typedef struct {
    temporal_scheduler_t s;
    audio_graph_t g;
    uint32_t done, bad;
    uint64_t frames;
} concurrent_t;
static void *consumer(void *arg)
{
    concurrent_t *c = arg;
    fake_t f = {0};
    while (!__atomic_load_n(&c->done, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&c->s.pending_ready, __ATOMIC_ACQUIRE)) {
        int r = frame(&c->s, &f, ++c->frames, 10);
        if (r != TC_OK || c->s.active.task[0].cpu_budget < 80 ||
            c->s.active.task[0].cpu_budget > 100) c->bad = 1;
    }
    return NULL;
}
static void test_handoff(void)
{
    concurrent_t c = {0}; graph(&c.g, 1); tc_scheduler_init(&c.s);
    temporal_contract_t t = task(1, 1000, 500, 100, TC_HARD, TC_MUTE);
    CHECK(tc_stage(&c.s, &c.g, &t, 1, &limits, NULL) == TC_OK);
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, consumer, &c) == 0);
    for (unsigned i = 0; i < 10000; ++i) {
        t.cpu_budget = i % 2 ? 80 : 100;
        int r;
        while ((r = tc_stage(&c.s, &c.g, &t, 1, &limits, NULL)) == TC_EBUSY) thrd_yield();
        CHECK(r == TC_OK);
    }
    __atomic_store_n(&c.done, 1u, __ATOMIC_RELEASE);
    CHECK(pthread_join(worker, NULL) == 0 && !c.bad && c.frames >= 10000);
}

static void test_period_change_after_gap(void)
{
    audio_graph_t g; graph(&g, 1);
    temporal_contract_t c = task(1, 4000, 500, 100, TC_HARD, TC_MUTE);
    temporal_scheduler_t s; tc_scheduler_init(&s);
    CHECK(tc_stage(&s, &g, &c, 1, &limits, NULL) == TC_OK);
    fake_t f = {0};
    CHECK(frame(&s, &f, 1, 10) == TC_OK);
    c.period = 1000;
    CHECK(tc_stage(&s, &g, &c, 1, &limits, NULL) == TC_OK);
    CHECK(frame(&s, &f, 9, 10) == TC_OK);
    /* Only old-plan frame 5 was missed. Frames 2..8 must NOT be recounted
     * using the newly adopted period, which becomes effective at frame 9. */
    CHECK(tc_state(&s, 1)->missed_releases == 1 && tc_state(&s, 1)->releases == 3);
}

static uint32_t rng = 0x615ab431u;
static uint32_t random_u32(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}
static void test_admission_execution_property(void)
{
    unsigned admitted = 0, rejected = 0;
    for (unsigned trial = 0; trial < 2000; ++trial) {
        unsigned n = 1 + random_u32() % 8;
        audio_graph_t g; graph(&g, n);
        temporal_contract_t c[8];
        fake_t f = {0}; f.output_cost = limits.job_overhead;
        for (unsigned i = 0; i < n; ++i) {
            uint64_t cost = 10 + random_u32() % 200;
            uint64_t deadline = cost + limits.job_overhead + random_u32() % 700;
            c[i] = task(i + 1, 1000 * (1 + random_u32() % 4), deadline, cost,
                        random_u32() % 3, TC_MUTE);
            f.cost[i + 1] = cost - 1; /* full allowed execution without crossing budget */
        }
        for (unsigned i = 1; i < n; ++i) {
            unsigned parent = random_u32() % i;
            if (c[i].period % c[parent].period == 0 && c[parent].criticality <= c[i].criticality)
                CHECK(audio_graph_connect(&g, (int)parent, (int)i) >= 0);
        }
        temporal_scheduler_t scheduler; tc_scheduler_init(&scheduler);
        int r = tc_stage(&scheduler, &g, c, n, &limits, NULL);
        if (r != TC_OK) { CHECK(r == TC_EADMISSION); ++rejected; continue; }
        ++admitted;
        for (unsigned seq = 1; seq <= 24; ++seq) {
            CHECK(frame(&scheduler, &f, seq, limits.frame_overhead) == TC_OK);
            CHECK(f.now <= (seq + 1u) * limits.frame_ticks);
            for (unsigned i = 0; i < n; ++i) {
                const tc_task_state_t *state = tc_state(&scheduler, c[i].pid);
                CHECK(state->deadline_misses == 0 && state->budget_overruns == 0);
                if (c[i].criticality != TC_BEST_EFFORT)
                    CHECK(state->completed == (seq - 1u) / (c[i].period / 1000) + 1u);
            }
        }
    }
    CHECK(admitted > 100 && rejected > 100);
    printf("admission/execution property: %u accepted, %u rejected graphs\n", admitted, rejected);
}

int main(void)
{
    test_admission(); test_dependencies(); test_rates_and_edf(); test_policies();
    test_lateness_and_recovery(); test_best_effort_and_updates(); test_handoff();
    test_period_change_after_gap(); test_admission_execution_property();
    printf("TEMPORAL HOST: PASS (%u checks; 10000 concurrent plan updates)\n", checks);
    return 0;
}
