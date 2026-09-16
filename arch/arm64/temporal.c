#include "temporal.h"
#include <stddef.h>

#define TC_TIME_MAX ((uint64_t)INT64_MAX / 4u)

static int fail(tc_admission_t *why, int code, uint32_t pid,
                 uint64_t required, uint64_t available)
{
    if (why) *why = (tc_admission_t){code, pid, required, available};
    return code;
}

static int index_of(const tc_plan_t *p, uint32_t pid)
{
    for (uint32_t i = 0; i < p->count; ++i)
        if (p->task[i].pid == pid) return (int)i;
    return -1;
}

static int before(const temporal_contract_t *a, const temporal_contract_t *b)
{
    if (a->criticality != b->criticality) return a->criticality < b->criticality;
    if (a->deadline != b->deadline) return a->deadline < b->deadline;
    return a->pid < b->pid;
}

int tc_plan_validate(const audio_graph_t *g, const temporal_contract_t *c,
                  uint32_t n, const tc_limits_t *l, tc_plan_t *out,
                  tc_admission_t *why)
{
    if (!g || !l || !out || (n && !c) || n > TC_MAX_TASKS ||
        !l->frame_ticks || l->frame_ticks > TC_TIME_MAX ||
        l->frame_overhead >= l->frame_ticks ||
        l->job_overhead > (l->frame_ticks - l->frame_overhead) / (n ? n : 1))
        return fail(why, TC_EINVAL, 0, 0, 0);
    tc_plan_t p = {0};
    p.limits = *l;
    p.count = n;
    uint32_t plugins = 0;
    for (int i = 0; i < GRAPH_MAX_NODES; ++i)
        if (g->nodes[i].type == NODE_PLUGIN) ++plugins;
    if (plugins != n) return fail(why, TC_ENODEV, 0, n, plugins);
    for (uint32_t i = 0; i < n; ++i) {
        const temporal_contract_t *t = &c[i];
        int node = audio_graph_node_by_pid(g, t->pid);
        if (!t->pid || t->pid == GRAPH_INPUT_PID || node < 0 ||
            g->nodes[node].type != NODE_PLUGIN)
            return fail(why, TC_ENODEV, t->pid, 0, 0);
        for (uint32_t j = 0; j < i; ++j)
            if (c[j].pid == t->pid) return fail(why, TC_EINVAL, t->pid, 0, 0);
        if (!t->period || t->period > TC_TIME_MAX || t->period % l->frame_ticks ||
            !t->deadline || t->deadline > l->frame_ticks ||
            t->deadline > t->period || !t->cpu_budget ||
            t->cpu_budget > t->deadline ||
            l->job_overhead > t->deadline - t->cpu_budget ||
            t->criticality > TC_BEST_EFFORT || t->overrun_policy > TC_DEGRADE ||
            (t->overrun_policy == TC_MUTE_THEN_KILL ? !t->kill_after : t->kill_after != 0) ||
            (t->overrun_policy == TC_DEGRADE ?
                 (!t->skip_periods || t->skip_periods > 1024 || t->criticality == TC_HARD) :
                 t->skip_periods != 0))
            return fail(why, TC_EINVAL, t->pid, t->cpu_budget, t->deadline);
        p.task[i] = *t;
    }
    for (int e = 0; e < GRAPH_MAX_EDGES; ++e) {
        const graph_edge_t *edge = &g->edges[e];
        if (!edge->used) continue;
        if (edge->src < 0 || edge->src >= GRAPH_MAX_NODES ||
            edge->dst < 0 || edge->dst >= GRAPH_MAX_NODES)
            return fail(why, TC_EDEPENDENCY, 0, 0, 0);
        const graph_node_t *src = &g->nodes[edge->src], *dst = &g->nodes[edge->dst];
        if (src->type == NODE_INPUT) continue;
        int a = index_of(&p, src->pid), b = index_of(&p, dst->pid);
        if (a < 0) return fail(why, TC_EDEPENDENCY, src->pid, 0, 0);
        if (dst->type == NODE_DAC) {
            if (p.task[a].period != l->frame_ticks)
                return fail(why, TC_EDEPENDENCY, src->pid, p.task[a].period, l->frame_ticks);
            continue;
        }
        if (b < 0) return fail(why, TC_EDEPENDENCY, dst->pid, 0, 0);
        if (edge->feedback) {
            if (p.task[a].period != l->frame_ticks || p.task[b].period != l->frame_ticks)
                return fail(why, TC_EDEPENDENCY, dst->pid, 0, 0);
            continue; /* explicitly previous-frame data */
        }
        if (p.task[b].period % p.task[a].period ||
            p.task[a].criticality > p.task[b].criticality)
            return fail(why, TC_EDEPENDENCY, dst->pid, p.task[a].period, p.task[b].period);
        p.deps[b] |= 1u << a;
    }
    uint32_t done = 0;
    for (uint32_t k = 0; k < n; ++k) {
        int best = -1;
        for (uint32_t i = 0; i < n; ++i)
            if (!(done & (1u << i)) && !(p.deps[i] & ~done) &&
                (best < 0 || before(&p.task[i], &p.task[best]))) best = (int)i;
        if (best < 0) return fail(why, TC_EDEPENDENCY, 0, 0, 0);
        p.order[k] = (uint32_t)best;
        done |= 1u << best;
    }
    *out = p;
    return fail(why, TC_OK, 0, 0, l->frame_ticks);
}

int tc_plan_build(const audio_graph_t *g, const temporal_contract_t *c,
                  uint32_t n, const tc_limits_t *l, tc_plan_t *out,
                  tc_admission_t *why)
{
    if (!out) return fail(why, TC_EINVAL, 0, 0, 0);
    tc_plan_t p;
    int rc = tc_plan_validate(g, c, n, l, &p, why);
    if (rc != TC_OK) return rc;
    uint64_t used = l->frame_overhead;
    for (uint32_t k = 0; k < n; ++k) {
        const temporal_contract_t *t = &p.task[p.order[k]];
        uint64_t cost = l->job_overhead;
        if (t->criticality != TC_BEST_EFFORT) cost += t->cpu_budget;
        if (cost > l->frame_ticks - used)
            return fail(why, TC_EADMISSION, t->pid, used + cost, l->frame_ticks);
        used += cost;
        if (t->criticality != TC_BEST_EFFORT && used > t->deadline)
            return fail(why, TC_EADMISSION, t->pid, used, t->deadline);
    }
    p.reserved_ticks = used;
    *out = p;
    return fail(why, TC_OK, 0, used, l->frame_ticks);
}

void tc_scheduler_init(temporal_scheduler_t *s)
{
    *s = (temporal_scheduler_t){0};
}

int tc_stage(temporal_scheduler_t *s, const audio_graph_t *g,
             const temporal_contract_t *c, uint32_t n, const tc_limits_t *l,
             tc_admission_t *why)
{
    if (!s || !l) return fail(why, TC_EINVAL, 0, 0, 0);
    if (__atomic_load_n(&s->pending_ready, __ATOMIC_ACQUIRE))
        return fail(why, TC_EBUSY, 0, 0, 0);
    if (s->configured_frame_ticks && s->configured_frame_ticks != l->frame_ticks)
        return fail(why, TC_EINVAL, 0, l->frame_ticks, s->configured_frame_ticks);
    tc_plan_t candidate;
    int r = tc_plan_build(g, c, n, l, &candidate, why);
    if (r != TC_OK) return r;
    s->pending = candidate;
    s->configured_frame_ticks = l->frame_ticks;
    __atomic_store_n(&s->pending_ready, 1u, __ATOMIC_RELEASE);
    return TC_OK;
}

static void add(uint64_t *v, uint64_t amount)
{
    *v = amount > UINT64_MAX - *v ? UINT64_MAX : *v + amount;
}

static void adopt(temporal_scheduler_t *s, uint64_t frame)
{
    if (!__atomic_load_n(&s->pending_ready, __ATOMIC_ACQUIRE)) return;
    tc_task_state_t old[TC_MAX_TASKS];
    for (uint32_t i = 0; i < TC_MAX_TASKS; ++i) old[i] = s->state[i];
    s->active = s->pending;
    for (uint32_t i = 0; i < TC_MAX_TASKS; ++i) {
        s->state[i] = (tc_task_state_t){0};
        if (i >= s->active.count) continue;
        s->state[i].pid = s->active.task[i].pid;
        s->state[i].activated_frame = frame;
        for (uint32_t j = 0; j < TC_MAX_TASKS; ++j)
            if (old[j].pid == s->state[i].pid) { s->state[i] = old[j]; break; }
    }
    s->active_valid = 1;
    __atomic_store_n(&s->pending_ready, 0u, __ATOMIC_RELEASE);
}

static void emit(const tc_ops_t *ops, tc_task_state_t *v, enum tc_output a)
{
    if (a == TC_OUTPUT_SILENCE) add(&v->muted, 1);
    if (a == TC_OUTPUT_BYPASS) add(&v->bypassed, 1);
    ops->output(ops->ctx, v->pid, a);
}

static enum tc_output fallback(const temporal_contract_t *t)
{
    return t->overrun_policy == TC_BYPASS ? TC_OUTPUT_BYPASS : TC_OUTPUT_SILENCE;
}

/* Count periodic releases in an inclusive frame range in constant time. */
static uint64_t due_between(uint64_t base, uint64_t a, uint64_t b, uint64_t period)
{
    if (a > b || b < base) return 0;
    uint64_t upper = (b - base) / period + 1;
    uint64_t lower = a > base ? (a - 1 - base) / period + 1 : 0;
    return upper - lower;
}

int tc_run_frame_masked(temporal_scheduler_t *s, uint64_t frame, uint64_t release,
                        const tc_ops_t *ops, uint32_t blocked)
{
    if (!s || !ops || !ops->clock || !ops->run || !ops->output || !ops->kill || !frame)
        return TC_EINVAL;
    if (s->started && frame <= s->last_frame) return TC_ESTALE;
    /* Validate timestamps before consuming a pending configuration. */
    uint64_t ticks = s->active_valid ? s->active.limits.frame_ticks : 0;
    if (!ticks && __atomic_load_n(&s->pending_ready, __ATOMIC_ACQUIRE))
        ticks = s->pending.limits.frame_ticks;
    if (!ticks) return TC_ENODEV;
    if (release > UINT64_MAX - ticks || ops->clock(ops->ctx) < release) return TC_EINVAL;
    if (s->started) {
        uint64_t delta = frame - s->last_frame;
        if (delta > UINT64_MAX / ticks || release < s->last_release ||
            release - s->last_release != delta * ticks) return TC_EINVAL;
    }
    /* Missed kicks belong to the plan that was active during the gap, not
     * the plan adopted below. Otherwise changing a period retroactively
     * changes release/miss counts and the degradation cooldown. */
    if (s->started) {
        for (uint32_t i = 0; i < s->active.count; ++i) {
            tc_task_state_t *v = &s->state[i];
            uint64_t period = s->active.task[i].period / ticks;
            uint64_t from = s->last_frame + 1;
            if (from < v->activated_frame) from = v->activated_frame;
            uint64_t missed = due_between(s->first_frame, from, frame - 1, period);
            add(&v->missed_releases, missed);
            add(&v->deadline_misses, missed);
            add(&v->releases, missed);
            v->skip_left -= missed < v->skip_left ? (uint32_t)missed : v->skip_left;
        }
    }
    adopt(s, frame);
    if (!s->started) { s->first_frame = frame; s->started = 1; }
    const tc_plan_t *p = &s->active;
    uint64_t frame_end = release + ticks;
    for (uint32_t k = 0; k < p->count; ++k) {
        uint32_t i = p->order[k];
        const temporal_contract_t *t = &p->task[i];
        tc_task_state_t *v = &s->state[i];
        uint64_t period = t->period / ticks;
        int due = ((frame - s->first_frame) % period == 0);
        if (due) add(&v->releases, 1);
        /* An unavailable predecessor is a dependency/deadline miss, not a
         * plugin CPU offence. Never bypass from an input not yet published. */
        if (blocked & (1u << i)) {
            if (due) {
                add(&v->shed, 1);
                if (t->criticality != TC_BEST_EFFORT) add(&v->deadline_misses, 1);
            }
            emit(ops, v, TC_OUTPUT_SILENCE);
            continue;
        }
        if (v->killed || !due) {
            emit(ops, v, v->killed ? TC_OUTPUT_SILENCE : fallback(t));
            continue;
        }
        if (v->skip_left) {
            --v->skip_left;
            add(&v->shed, 1);
            emit(ops, v, fallback(t));
            continue;
        }
        uint64_t now = ops->clock(ops->ctx);
        uint64_t deadline = release + t->deadline;
        uint64_t cutoff = deadline - p->limits.job_overhead;
        /* Reserve bounded output work for ALL remaining nodes, including
         * jobs which will be skipped. No optional job can consume that tail. */
        uint64_t tail = (p->count - k) * p->limits.job_overhead;
        if (cutoff > frame_end - tail) cutoff = frame_end - tail;
        if (now >= cutoff || t->cpu_budget > cutoff - now) {
            /* Worker lateness/optional load is not proof of a plugin offence:
             * never kill a plugin merely because it was dispatched too late. */
            if (t->criticality != TC_BEST_EFFORT) add(&v->deadline_misses, 1);
            add(&v->shed, 1);
            emit(ops, v, fallback(t));
            continue;
        }
        add(&v->runs, 1);
        long result = ops->run(ops->ctx, t->pid, t->cpu_budget, cutoff);
        uint64_t end = ops->clock(ops->ctx);
        uint64_t elapsed = end >= now ? end - now : 0;
        add(&v->service_ticks, elapsed);
        if (elapsed > v->service_max) v->service_max = elapsed;
        if (end < now) result = -1; /* broken clock fails closed */
        if (result == TC_RUN_DEADLINE || end > deadline) add(&v->deadline_misses, 1);
        if (result < 0 && result != TC_RUN_BUDGET && result != TC_RUN_DEADLINE) {
            add(&v->faults, 1);
            v->killed = 1;
            emit(ops, v, TC_OUTPUT_SILENCE);
            ops->kill(ops->ctx, t->pid);
        } else if (result == TC_RUN_BUDGET ||
                   (result >= 0 && elapsed >= t->cpu_budget)) {
            add(&v->budget_overruns, 1);
            if (v->streak < UINT32_MAX) ++v->streak;
            if (t->overrun_policy == TC_KILL ||
                (t->overrun_policy == TC_MUTE_THEN_KILL && v->streak >= t->kill_after))
                v->killed = 1;
            if (t->overrun_policy == TC_DEGRADE) {
                v->skip_left = t->skip_periods;
                add(&v->degraded, 1);
            }
            emit(ops, v, v->killed ? TC_OUTPUT_SILENCE : fallback(t));
            if (v->killed) ops->kill(ops->ctx, t->pid);
        } else if (result == TC_RUN_DEADLINE || end > deadline) {
            emit(ops, v, fallback(t));
        } else {
            v->streak = 0;
            add(&v->completed, 1);
            emit(ops, v, TC_OUTPUT_OK);
        }
    }
    s->last_frame = frame;
    s->last_release = release;
    return TC_OK;
}

const tc_task_state_t *tc_state(const temporal_scheduler_t *s, uint32_t pid)
{
    if (!s || !pid) return NULL;
    for (uint32_t i = 0; i < s->active.count; ++i)
        if (s->state[i].pid == pid) return &s->state[i];
    return NULL;
}

int tc_run_frame(temporal_scheduler_t *s, uint64_t frame, uint64_t release,
                 const tc_ops_t *ops)
{
    return tc_run_frame_masked(s, frame, release, ops, 0);
}
