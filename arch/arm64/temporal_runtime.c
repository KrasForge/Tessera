#include "temporal_runtime.h"
#include "el0_context.h"
#include "plugin_abi.h"
#include <stddef.h>

static int enter(temporal_runtime_t *r, int paused)
{
    if (!r || !r->initialized) return TC_EINVAL;
    uint32_t idle = 0;
    if (!__atomic_compare_exchange_n(&r->editing, &idle, 1u, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return TC_EBUSY;
    r->editor_cpu = el0_cpu_index();
    if (paused && (!aw_paused(r->worker) || r->running)) {
        __atomic_store_n(&r->editing, 0u, __ATOMIC_RELEASE);
        return TC_EBUSY;
    }
    return TC_OK;
}
static void leave(temporal_runtime_t *r)
{ __atomic_store_n(&r->editing, 0u, __ATOMIC_RELEASE); }
static int may_mutate(void *ctx)
{
    temporal_runtime_t *r = ctx;
    return __atomic_load_n(&r->editing, __ATOMIC_ACQUIRE) &&
        r->editor_cpu == el0_cpu_index() && !r->running && aw_paused(r->worker);
}

int tr_init(temporal_runtime_t *r, plugin_mgr_t *m, audio_worker_t *w,
             const tc_limits_t *limits, uint64_t (*clock)(void),
             uint32_t rate, uint32_t frames, uint64_t lifecycle_ticks,
             const tr_io_ops_t *io)
{
    if (!r || !m || !m->gc || !w || !clock || !limits || !limits->frame_ticks ||
        !rate || !frames || frames > 4096 || !lifecycle_ticks ||
        lifecycle_ticks > INT64_MAX || !io || !io->make_binding ||
        m->gc->mutation_guard || w->online || w->stop || w->n_nodes) return TC_EINVAL;
    for (unsigned i = 0; i < PM_MAX_PLUGINS; ++i)
        if (m->slots[i].used) return TC_EBUSY;
    *r = (temporal_runtime_t){0};
    r->manager = m; r->worker = w; r->io = *io;
    r->sample_rate = rate; r->frames = frames; r->lifecycle_ticks = lifecycle_ticks;
    tc_host_init(&r->host, m->gc, limits, clock);
    int rc = tc_host_attach(&r->host, w);
    if (rc != TC_OK) return rc;
    aw_pause(w);
    pm_set_lifecycle_budget(m, lifecycle_ticks);
    r->initialized = 1;
    gc_set_mutation_guard(m->gc, may_mutate, r);
    return TC_OK;
}

int tr_pause(temporal_runtime_t *r, uint64_t timeout)
{
    if (!timeout || timeout > INT64_MAX) return TC_EINVAL;
    int rc = enter(r, 0);
    if (rc != TC_OK) return rc;
    aw_pause(r->worker);
    uint64_t start = r->host.clock();
    while (!aw_paused(r->worker)) {
        if (r->host.clock() - start >= timeout) { leave(r); return TR_ETIMEOUT; }
    }
    r->running = 0;
    /* The worker cannot consume or execute after the producer handshake.
     * desired already holds any accepted pending update; start will restage. */
    __atomic_store_n(&r->host.scheduler.pending_ready, 0u, __ATOMIC_RELEASE);
    leave(r);
    return TC_OK;
}

int tr_start(temporal_runtime_t *r)
{
    int rc = enter(r, 1);
    if (rc != TC_OK) return rc;
    if (!r->host.desired_count) { leave(r); return TC_ENODEV; }
    temporal_scheduler_t *s = &r->host.scheduler;
    __atomic_store_n(&s->pending_ready, 0u, __ATOMIC_RELEASE);
    rc = tc_host_stage(&r->host, r->host.desired, r->host.desired_count);
    if (rc == TC_OK) {
        s->started = 0; s->first_frame = 0; s->last_frame = 0; s->last_release = 0;
        for (unsigned i = 0; i < TC_MAX_TASKS; ++i) s->state[i].activated_frame = 0;
        r->running = 1;
        if (aw_resume(r->worker)) { r->running = 0; rc = TC_EBUSY; }
    }
    leave(r);
    return rc;
}

long tr_load(temporal_runtime_t *r, const char *path, const temporal_contract_t *contract)
{
    if (!path || !contract) return TC_EINVAL;
    int rc = enter(r, 1);
    if (rc != TC_OK) return rc;
    if (r->host.desired_count >= TC_MAX_TASKS) { leave(r); return TC_EINVAL; }
    long pid = pm_load(r->manager, path);
    if (pid < 0) { r->last_lifecycle_result = pid; leave(r); return TR_EPLUGIN; }
    plugin_t *pl = pm_plugin(r->manager, (uint32_t)pid);
    tc_plugin_binding_t b = {0};
    b.call.plugin = pl;
    int made = 1, bound = 0;
    if (r->io.make_binding(r->io.ctx, pl, &b) || b.call.plugin != pl ||
        b.call.frames != r->frames) { rc = TR_EIO; goto rollback; }
    r->last_lifecycle_result = plugin_call_init(pl, r->sample_rate, r->frames);
    if (r->last_lifecycle_result != TESSERA_PLUGIN_OK) { rc = TR_EPLUGIN; goto rollback; }
    rc = tc_host_bind(&r->host, &b);
    if (rc != TC_OK) goto rollback;
    bound = 1;
    temporal_contract_t next[TC_MAX_TASKS];
    unsigned count = r->host.desired_count;
    for (unsigned i = 0; i < count; ++i) next[i] = r->host.desired[i];
    next[count] = *contract; next[count].pid = (uint32_t)pid;
    tc_plan_t plan;
    rc = tc_plan_build(&r->manager->gc->graph, next, count + 1, &r->host.limits,
                       &plan, &r->host.last_admission);
    if (rc != TC_OK) goto rollback;
    r->host.desired[count] = next[count];
    r->host.desired_count = count + 1;
    r->host.scheduler.active_valid = 0;
    leave(r);
    return pid;
rollback:
    if (bound) tc_host_unbind(&r->host, (uint32_t)pid);
    if (pl->proc->state != PROC_KILLED) (void)plugin_call_destroy(pl);
    if (made && r->io.release_binding) r->io.release_binding(r->io.ctx, &b);
    (void)pm_unload(r->manager, (uint32_t)pid);
    leave(r);
    return rc;
}

static int unload_locked(temporal_runtime_t *r, uint32_t pid)
{
    plugin_t *pl = pm_plugin(r->manager, pid);
    if (!pl) return TC_ENODEV;
    tc_plugin_binding_t binding = {0};
    int found = 0;
    for (unsigned i = 0; i < r->host.bindings; ++i)
        if (r->host.binding[i].call.plugin == pl) { binding = r->host.binding[i]; found = 1; break; }
    if (!found) return TC_ENODEV;
    int rc = tc_host_unbind(&r->host, pid);
    if (rc != TC_OK) return rc;
    long result = pl->proc->state == PROC_KILLED ? 0 : plugin_call_destroy(pl);
    if (result < 0) r->last_lifecycle_result = result;
    if (r->io.release_binding) r->io.release_binding(r->io.ctx, &binding);
    if (pm_unload(r->manager, pid) != PM_OK) return TR_EPLUGIN;
    return result < 0 ? TR_EPLUGIN : TC_OK;
}
int tr_unload(temporal_runtime_t *r, uint32_t pid)
{
    int rc = enter(r, 1);
    if (rc != TC_OK) return rc;
    rc = unload_locked(r, pid);
    leave(r);
    return rc;
}

static int wire(temporal_runtime_t *r, uint32_t src, uint32_t dst, int mode)
{
    int rc = enter(r, 1);
    if (rc != TC_OK) return rc;
    audio_graph_t next = r->manager->gc->graph;
    int a = audio_graph_node_by_pid(&next, src), b = audio_graph_node_by_pid(&next, dst);
    if (a < 0 || b < 0) { leave(r); return TC_ENODEV; }
    if (mode == 2) {
        if (audio_graph_find_edge(&next, a, b) < 0) { leave(r); return TC_ENODEV; }
        audio_graph_disconnect(&next, a, b);
    } else if ((mode ? audio_graph_connect_feedback(&next, a, b) :
                       audio_graph_connect(&next, a, b)) < 0) { leave(r); return TC_EDEPENDENCY; }
    tc_plan_t plan;
    rc = tc_plan_build(&next, r->host.desired, r->host.desired_count, &r->host.limits,
                       &plan, &r->host.last_admission);
    if (rc == TC_OK) {
        graph_control_t *gc = r->manager->gc;
        rc = mode == 2 ? gc_disconnect(gc, src, dst) :
             mode ? gc_connect_feedback(gc, src, dst) : gc_connect(gc, src, dst);
        if (rc == GC_OK) r->host.scheduler.active_valid = 0;
    }
    leave(r);
    return rc;
}
int tr_connect(temporal_runtime_t *r, uint32_t src, uint32_t dst, int feedback)
{ return feedback < 0 || feedback > 1 ? TC_EINVAL : wire(r, src, dst, feedback); }
int tr_disconnect(temporal_runtime_t *r, uint32_t src, uint32_t dst)
{ return wire(r, src, dst, 2); }
int tr_set_contract(temporal_runtime_t *r, const temporal_contract_t *contract)
{
    int rc = enter(r, 0);
    if (rc != TC_OK) return rc;
    rc = tc_host_set(&r->host, contract);
    leave(r);
    return rc;
}
int tr_shutdown(temporal_runtime_t *r, uint64_t timeout)
{
    int rc = tr_pause(r, timeout);
    if (rc != TC_OK) return rc;
    rc = enter(r, 1);
    if (rc != TC_OK) return rc;
    while (r->host.bindings) {
        uint32_t before = r->host.bindings;
        rc = unload_locked(r, r->host.binding[0].call.plugin->proc->pid);
        if (r->host.bindings == before) { leave(r); return rc; }
    }
    rc = tc_host_detach(&r->host);
    if (rc == TC_OK) {
        gc_set_mutation_guard(r->manager->gc, NULL, NULL);
        r->initialized = 0;
    }
    leave(r);
    return rc;
}
