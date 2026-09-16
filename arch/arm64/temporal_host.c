#include "temporal_host.h"
#include "process.h"
#include "usermode.h"
#include <stddef.h>
#include "el0_context.h"

static temporal_host_t *control_host;
static temporal_host_t *execution_host[EL0_CONTEXT_CPUS];

void tc_host_init(temporal_host_t *h, graph_control_t *gc,
                   const tc_limits_t *l, uint64_t (*clock)(void))
{
    *h = (temporal_host_t){0};
    h->gc = gc;
    if (l) h->limits = *l;
    h->clock = clock;
    tc_scheduler_init(&h->scheduler);
}

static tc_plugin_binding_t *binding_for(temporal_host_t *h, uint32_t pid)
{
    for (uint32_t i = 0; i < h->bindings; ++i)
        if (h->binding[i].call.plugin->proc->pid == pid) return &h->binding[i];
    return NULL;
}

int tc_host_bind(temporal_host_t *h, const tc_plugin_binding_t *b)
{
    if (!h || !b || !b->call.plugin || !b->call.plugin->proc ||
        !b->call.left || !b->call.right || !b->call.frames ||
        b->call.plugin->proc->svc_gate != PLUGIN_TRAMP_VA ||
        b->call.frames > 4096 || h->bindings >= TC_MAX_TASKS ||
        __atomic_load_n(&b->call.plugin->host_refs, __ATOMIC_ACQUIRE) ||
        binding_for(h, b->call.plugin->proc->pid)) return TC_EINVAL;
    if (__atomic_load_n(&h->scheduler.pending_ready, __ATOMIC_ACQUIRE) ||
        (h->worker && !aw_quiescent(h->worker))) return TC_EBUSY;
    uint32_t unbound = 0;
    if (!__atomic_compare_exchange_n(&b->call.plugin->host_refs, &unbound, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return TC_EBUSY;
    h->binding[h->bindings++] = *b;
    return TC_OK;
}

int tc_host_unbind(temporal_host_t *h, uint32_t pid)
{
    if (!h || !pid) return TC_EINVAL;
    if (__atomic_load_n(&h->scheduler.pending_ready, __ATOMIC_ACQUIRE) ||
        (h->worker && !aw_quiescent(h->worker))) return TC_EBUSY;
    uint32_t slot = h->bindings;
    for (uint32_t i = 0; i < h->bindings; ++i)
        if (h->binding[i].call.plugin->proc->pid == pid) { slot = i; break; }
    if (slot == h->bindings) return TC_ENODEV;
    /* Do not keep a binding whose plugin/process/output pages will be freed.
     * With no admitted replacement, host_frame fails closed to silence. */
    h->scheduler.active_valid = 0;
    __atomic_fetch_sub(&h->binding[slot].call.plugin->host_refs, 1u, __ATOMIC_ACQ_REL);
    for (uint32_t i = slot + 1; i < h->bindings; ++i)
        h->binding[i - 1] = h->binding[i];
    h->binding[--h->bindings] = (tc_plugin_binding_t){0};
    for (uint32_t i = 0; i < h->desired_count; ++i) {
        if (h->desired[i].pid != pid) continue;
        for (uint32_t j = i + 1; j < h->desired_count; ++j)
            h->desired[j - 1] = h->desired[j];
        h->desired[--h->desired_count] = (temporal_contract_t){0};
        break;
    }
    return TC_OK;
}

int tc_host_stage(temporal_host_t *h, const temporal_contract_t *c, uint32_t n)
{
    if (!h || !h->gc || !h->clock || n > TC_MAX_TASKS || (n && !c)) return TC_EINVAL;
    for (uint32_t i = 0; i < n; ++i) {
        tc_plugin_binding_t *b = binding_for(h, c[i].pid);
        if (!b) return TC_ENODEV;
        if (c[i].overrun_policy == TC_BYPASS && (!b->input_left || !b->input_right))
            return TC_EINVAL;
    }
    int r = tc_stage(&h->scheduler, &h->gc->graph, c, n, &h->limits, &h->last_admission);
    if (r == TC_OK) {
        for (uint32_t i = 0; i < n; ++i) h->desired[i] = c[i];
        h->desired_count = n;
    }
    return r;
}

int tc_host_set(temporal_host_t *h, const temporal_contract_t *c)
{
    if (!h || !c) return TC_EINVAL;
    temporal_contract_t next[TC_MAX_TASKS];
    int found = 0;
    for (uint32_t i = 0; i < h->desired_count; ++i) {
        next[i] = h->desired[i];
        if (next[i].pid == c->pid) { next[i] = *c; found = 1; }
    }
    return found ? tc_host_stage(h, next, h->desired_count) : TC_ENODEV;
}

static uint64_t host_clock(void *ctx)
{
    return ((temporal_host_t *)ctx)->clock();
}

static long host_run(void *ctx, uint32_t pid, uint64_t budget, uint64_t cutoff)
{
    tc_plugin_binding_t *b = binding_for(ctx, pid);
    return b ? budget_plugin_invoke(&b->call, budget, cutoff, NULL) : -1;
}

static void host_output(void *ctx, uint32_t pid, enum tc_output action)
{
    tc_plugin_binding_t *b = binding_for(ctx, pid);
    if (!b) return;
    if (action != TC_OUTPUT_OK) {
        uint32_t *left = b->call.left, *right = b->call.right;
        const uint32_t *il = b->input_left, *ir = b->input_right;
        int dry = action == TC_OUTPUT_BYPASS && il && ir;
        for (uint32_t i = 0; i < b->call.frames; ++i) {
            left[i] = dry ? il[i] : 0;
            right[i] = dry ? ir[i] : 0;
        }
    }
    if (b->publish) b->publish(b->ctx, pid, action);
}

static void host_kill(void *ctx, uint32_t pid)
{
    tc_plugin_binding_t *b = binding_for(ctx, pid);
    if (b) process_kill(b->call.plugin->proc, BUDGET_PREEMPTED);
}

static void host_frame(void *ctx)
{
    temporal_host_t *h = ctx;
    tc_ops_t ops = {host_clock, host_run, host_output, host_kill, h};
    h->last_result = tc_run_frame(&h->scheduler, h->worker->block_seq,
                                   h->worker->release_ticks, &ops);
    /* Invalid cadence metadata must not leave the previous block published. */
    if (h->last_result != TC_OK)
        for (uint32_t i = 0; i < h->bindings; ++i)
            host_output(h, h->binding[i].call.plugin->proc->pid, TC_OUTPUT_SILENCE);
}

int tc_host_attach(temporal_host_t *h, audio_worker_t *w)
{
    if (!h || !w || !h->clock || h->worker || w->n_nodes || w->online ||
        w->cpu_id == 0 || w->cpu_id > 3) return TC_EINVAL;
    if (execution_host[w->cpu_id] && execution_host[w->cpu_id] != h) return TC_EBUSY;
    int r = aw_assign(w, host_frame, h);
    if (r < 0) return TC_EBUSY;
    h->worker = w;
    execution_host[w->cpu_id] = h;
    return TC_OK;
}

int tc_host_detach(temporal_host_t *h)
{
    if (!h || !h->worker) return TC_EINVAL;
    audio_worker_t *w = h->worker;
    if (!aw_quiescent(w)) return TC_EBUSY;
    aw_clear(w);
    h->worker = NULL;
    if (execution_host[w->cpu_id] == h) execution_host[w->cpu_id] = NULL;
    temporal_host_t *expected = h;
    (void)__atomic_compare_exchange_n(&control_host, &expected, NULL, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    return TC_OK;
}

void tc_host_bind_control(temporal_host_t *h) { __atomic_store_n(&control_host, h, __ATOMIC_RELEASE); }

long sys_plugin_set_contract(uint32_t pid, uint64_t period, uint64_t deadline,
                              uint64_t budget, uint64_t flags, uint64_t argument)
{
    temporal_host_t *h = __atomic_load_n(&control_host, __ATOMIC_ACQUIRE);
    if (!h) return TC_ENODEV;
    if (flags > 0xffffu || argument > UINT32_MAX) return TC_EINVAL;
    temporal_contract_t c = {0};
    c.pid = pid;
    c.period = period;
    c.deadline = deadline;
    c.cpu_budget = budget;
    c.criticality = (uint32_t)(flags & 0xffu);
    c.overrun_policy = (uint32_t)(flags >> 8);
    if (c.overrun_policy == TC_DEGRADE) c.skip_periods = (uint32_t)argument;
    else c.kill_after = (uint32_t)argument;
    return h->control_set ? h->control_set(h->control_ctx, &c) : tc_host_set(h, &c);
}
