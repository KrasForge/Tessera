#include "temporal_multicore.h"
#include <stddef.h>

static int reject(tc_admission_t *why, int code, uint32_t pid, uint64_t need,
                  uint64_t have) {
  if (why)
    *why = (tc_admission_t){code, pid, need, have};
  return code;
}
static uint64_t max64(uint64_t a, uint64_t b) { return a > b ? a : b; }
int tm_plan_build(const audio_graph_t *g, const temporal_contract_t *tasks,
                  uint32_t n, const tm_limits_t *limits, uint32_t mask,
                  const uint8_t *pins, tm_plan_t *out, tc_admission_t *why) {
  if (!out || !limits || !mask || (mask & ~14u) ||
      limits->handoff_ticks > limits->time.frame_ticks ||
      limits->time.job_overhead > limits->time.frame_ticks / 2)
    return reject(why, TC_EINVAL, 0, 0, 0);
  tm_plan_t p = {0};
  int rc = tc_plan_validate(g, tasks, n, &limits->time, &p.graph, why);
  if (rc != TC_OK)
    return rc;
  p.limits = *limits;
  p.core_mask = mask;
  uint64_t ready[TM_CORES], cost[TC_MAX_TASKS];
  for (unsigned core = 0; core < TM_CORES; ++core)
    ready[core] = limits->time.frame_overhead;
  for (uint32_t k = 0; k < n; ++k) {
    uint32_t i = p.graph.order[k];
    const temporal_contract_t *t = &tasks[i];
    unsigned pin = pins ? pins[i] : 0;
    if (pin > TM_CORES || (pin && !(mask & (1u << pin))))
      return reject(why, TC_EINVAL, t->pid, pin, mask);
    cost[i] = limits->time.job_overhead * 2;
    if (t->criticality != TC_BEST_EFFORT)
      cost[i] += t->cpu_budget;
    uint64_t best_finish = UINT64_MAX, best_start = 0;
    unsigned best_core = 0;
    for (unsigned cpu = 1; cpu <= TM_CORES; ++cpu) {
      if (!(mask & (1u << cpu)) || (pin && pin != cpu))
        continue;
      uint64_t start = ready[cpu - 1];
      for (unsigned j = 0; j < n; ++j)
        if (p.graph.deps[i] & (1u << j))
          start =
              max64(start, p.finish[j] +
                               (p.core[j] != cpu ? limits->handoff_ticks : 0));
      uint64_t finish = start + cost[i];
      if (finish < best_finish) {
        best_finish = finish;
        best_start = start;
        best_core = cpu;
      }
    }
    uint64_t end = t->criticality == TC_BEST_EFFORT ? limits->time.frame_ticks
                                                    : t->deadline;
    if (!best_core || best_finish > end)
      return reject(why, TC_EADMISSION, t->pid, best_finish, end);
    p.core[i] = (uint8_t)best_core;
    p.start[i] = best_start;
    p.finish[i] = best_finish;
    ready[best_core - 1] = best_finish;
    for (unsigned j = 0; j < n; ++j)
      if ((p.graph.deps[i] & (1u << j)) && p.core[j] != best_core)
        ++p.cross_edges;
  }
  /* Propagate latest safe finishes backwards through BOTH graph edges and
   * the chosen per-core sequence. A soft/optional job cannot consume a
   * successor's reservation just because its own deadline is much later. */
  uint64_t next[TM_CORES];
  for (unsigned cpu = 0; cpu < TM_CORES; ++cpu) {
    next[cpu] = limits->time.frame_ticks;
    p.core_reserved[cpu] = ready[cpu];
  }
  for (unsigned k = n; k > 0; --k) {
    unsigned i = p.graph.order[k - 1], cpu = p.core[i];
    uint64_t end = tasks[i].deadline;
    if (next[cpu - 1] < end)
      end = next[cpu - 1];
    for (unsigned j = 0; j < n; ++j)
      if (p.graph.deps[j] & (1u << i)) {
        uint64_t transfer = p.core[j] != cpu ? limits->handoff_ticks : 0;
        uint64_t start = p.latest_finish[j] - cost[j];
        if (start < transfer)
          return reject(why, TC_EADMISSION, tasks[i].pid, transfer, start);
        if (start - transfer < end)
          end = start - transfer;
      }
    if (end < p.finish[i])
      return reject(why, TC_EADMISSION, tasks[i].pid, p.finish[i], end);
    p.latest_finish[i] = end;
    next[cpu - 1] = end - cost[i];
  }
  *out = p;
  return reject(why, TC_OK, 0, 0, limits->time.frame_ticks);
}

#define STATE_FIELDS(X)                                                        \
  X(pid)                                                                       \
  X(killed) X(streak) X(skip_left) X(releases) X(runs) X(completed)            \
      X(budget_overruns) X(deadline_misses) X(missed_releases) X(shed)         \
          X(muted) X(bypassed) X(degraded) X(faults) X(service_ticks)          \
              X(service_max) X(activated_frame)
static const tm_plan_t *plan(const tm_runtime_t *r) {
  return &r->plans[__atomic_load_n(&r->active_plan, __ATOMIC_ACQUIRE)];
}
static tm_job_t *job_at(tm_runtime_t *r, unsigned i) {
  return &r->jobs[r->mapping[r->active_plan][i]];
}
static void publish_stats(tm_job_t *job) {
  uint32_t seq = __atomic_load_n(&job->stat_seq, __ATOMIC_RELAXED);
  __atomic_store_n(&job->stat_seq, seq + 1, __ATOMIC_SEQ_CST);
#define STORE(f)                                                               \
  __atomic_store_n(&job->published.f, job->scheduler.state[0].f,               \
                   __ATOMIC_SEQ_CST);
  STATE_FIELDS(STORE)
#undef STORE
  __atomic_store_n(&job->stat_seq, seq + 2, __ATOMIC_SEQ_CST);
}
int tm_snapshot(const tm_runtime_t *r, uint32_t pid, tc_task_state_t *out) {
  if (!r || !out || !pid)
    return TC_EINVAL;
  unsigned bank = __atomic_load_n(&r->active_plan, __ATOMIC_ACQUIRE);
  const tm_plan_t *p = &r->plans[bank];
  for (unsigned i = 0; i < p->graph.count; ++i)
    if (p->graph.task[i].pid == pid) {
      const tm_job_t *job = &r->jobs[r->mapping[bank][i]];
      for (unsigned retry = 0; retry < 8; ++retry) {
        uint32_t seq = __atomic_load_n(&job->stat_seq, __ATOMIC_SEQ_CST);
        if (seq & 1)
          continue;
        tc_task_state_t copy;
#define LOAD(f) copy.f = __atomic_load_n(&job->published.f, __ATOMIC_SEQ_CST);
        STATE_FIELDS(LOAD)
#undef LOAD
        if (seq == __atomic_load_n(&job->stat_seq, __ATOMIC_SEQ_CST)) {
          *out = copy;
          return TC_OK;
        }
      }
      return TC_EBUSY;
    }
  return TC_ENODEV;
}
static uint64_t task_clock(void *ctx) {
  tm_runtime_t *r = ((tm_task_ctx_t *)ctx)->runtime;
  return r->ops.clock(r->ops.ctx);
}
static long task_run(void *ctx, uint32_t pid, uint64_t budget,
                     uint64_t cutoff) {
  tm_runtime_t *r = ((tm_task_ctx_t *)ctx)->runtime;
  return r->ops.run(r->ops.ctx, pid, budget, cutoff);
}
static void task_output(void *ctx, uint32_t pid, enum tc_output action) {
  tm_task_ctx_t *t = ctx;
  tm_runtime_t *r = t->runtime;
  r->ops.output(r->ops.ctx, pid, r->bank, action);
  __atomic_store_n(&job_at(r, t->index)->done, r->frame, __ATOMIC_RELEASE);
}
static void task_kill(void *ctx, uint32_t pid) {
  tm_runtime_t *r = ((tm_task_ctx_t *)ctx)->runtime;
  r->ops.kill(r->ops.ctx, pid);
}
static int dependencies_ready(tm_runtime_t *r, unsigned i) {
  const tm_plan_t *p = plan(r);
  uint32_t deps = p->graph.deps[i];
  for (unsigned j = 0; j < p->graph.count; ++j)
    if ((deps & (1u << j)) &&
        __atomic_load_n(&job_at(r, j)->done, __ATOMIC_ACQUIRE) != r->frame)
      return 0;
  return 1;
}
static void adopt_job(tm_runtime_t *r, unsigned i) {
  tm_job_t *j = job_at(r, i);
  if (j->generation == r->generation)
    return;
  temporal_scheduler_t *s = &j->scheduler;
  const tm_plan_t *p = plan(r);
  int reset = !j->generation || j->epoch_version != r->epoch_version;
  if (reset) {
    /* New instances join this epoch, but no release before activation is
     * charged to them. Explicit pause/resume starts a new common phase. */
    s->started = r->frame > r->epoch;
    s->first_frame = r->epoch;
    s->last_frame = s->started ? r->frame - 1 : 0;
    s->last_release = s->started ? r->release - p->limits.time.frame_ticks : 0;
    s->state[0].activated_frame = r->frame;
  }
  /* Stage only here, after global configuration publication. A control
   * writer must not make an individual job adopt early. tc_run_frame counts
   * missed releases under the OLD period before adopting this new period. */
  s->pending.count = 1;
  s->pending.limits = p->limits.time;
  s->pending.task[0] = p->graph.task[i];
  s->pending.task[0].deadline = p->latest_finish[i];
  s->pending.order[0] = s->pending.deps[0] = 0;
  __atomic_store_n(&s->pending_ready, 1u, __ATOMIC_RELEASE);
  j->generation = r->generation;
  j->epoch_version = r->epoch_version;
}
static void execute_core(void *ctx) {
  tm_worker_ctx_t *w = ctx;
  tm_runtime_t *r = w->runtime;
  const tm_plan_t *p = plan(r);
  for (unsigned k = 0; k < p->graph.count; ++k) {
    unsigned i = p->graph.order[k];
    if (p->core[i] != w->cpu)
      continue;
    const temporal_contract_t *t = &p->graph.task[i];
    adopt_job(r, i);
    uint64_t need = p->limits.time.job_overhead * 2;
    if (t->criticality != TC_BEST_EFFORT)
      need += t->cpu_budget;
    uint64_t wait_end = r->release + p->latest_finish[i] - need;
    int ready;
    while (!(ready = dependencies_ready(r, i)) &&
           r->ops.clock(r->ops.ctx) < wait_end) {
#if defined(__aarch64__) && !defined(HOSTTEST)
      __asm__ volatile("yield" ::: "memory");
#endif
    }
    if (ready && r->ops.prepare)
      ready =
          r->ops.prepare(r->ops.ctx, t->pid, r->bank, r->previous_valid) == 0;
    tc_ops_t ops = {task_clock, task_run, task_output, task_kill,
                    &r->task_ctx[i]};
    int rc = tc_run_frame_masked(&job_at(r, i)->scheduler, r->frame, r->release,
                                 &ops, ready ? 0 : 1);
    if (rc != TC_OK)
      task_output(&r->task_ctx[i], t->pid, TC_OUTPUT_SILENCE);
    publish_stats(job_at(r, i));
  }
}
int tm_init(tm_runtime_t *r, audio_worker_t *workers[TM_CORES],
            const tm_ops_t *ops) {
  if (!r || !workers || !ops || !ops->clock || !ops->run || !ops->output ||
      !ops->kill)
    return TC_EINVAL;
  for (unsigned i = 0; i < TM_CORES; ++i)
    if (!workers[i] || workers[i]->cpu_id != i + 1 || workers[i]->n_nodes ||
        workers[i]->online || workers[i]->stop)
      return TC_EBUSY;
  *r = (tm_runtime_t){0};
  r->ops = *ops;
  r->gate = 2;
  for (unsigned i = 0; i < TC_MAX_TASKS; ++i)
    r->task_ctx[i] = (tm_task_ctx_t){r, i};
  for (unsigned i = 0; i < TM_CORES; ++i) {
    r->worker[i] = workers[i];
    r->worker_ctx[i] = (tm_worker_ctx_t){r, i + 1};
    if (aw_assign(workers[i], execute_core, &r->worker_ctx[i]) < 0)
      return TC_EBUSY;
  }
  return TC_OK;
}
int tm_drained(const tm_runtime_t *r) {
  for (unsigned i = 0; i < TM_CORES; ++i)
    if (!aw_drained(r->worker[i]))
      return 0;
  return 1;
}
void tm_request_pause(tm_runtime_t *r) {
  __atomic_fetch_or(&r->gate, 2u, __ATOMIC_SEQ_CST);
}
int tm_paused(const tm_runtime_t *r) {
  return __atomic_load_n(&r->gate, __ATOMIC_SEQ_CST) == 2 && tm_drained(r);
}
int tm_prepare(tm_runtime_t *r, const tm_plan_t *p) {
  if (!r || !p || p->graph.count > TC_MAX_TASKS)
    return TC_EINVAL;
  if (__atomic_load_n(&r->prepared_plan, __ATOMIC_ACQUIRE))
    return TC_EBUSY;
  unsigned bank = __atomic_load_n(&r->active_plan, __ATOMIC_ACQUIRE),
           next = bank ^ 1;
  uint32_t used = 0, active_jobs = 0;
  if (r->valid)
    for (unsigned i = 0; i < r->plans[bank].graph.count; ++i)
      active_jobs |= 1u << r->mapping[bank][i];
  for (unsigned i = 0; i < p->graph.count; ++i) {
    unsigned chosen = TC_MAX_TASKS * 2;
    for (unsigned j = 0; j < TC_MAX_TASKS * 2; ++j)
      if ((active_jobs & (1u << j)) &&
          r->jobs[j].owner_pid == p->graph.task[i].pid) {
        chosen = j;
        break;
      }
    if (chosen == TC_MAX_TASKS * 2) {
      for (unsigned j = 0; j < TC_MAX_TASKS * 2; ++j)
        if (!((active_jobs | used) & (1u << j))) {
          chosen = j;
          break;
        }
      if (chosen == TC_MAX_TASKS * 2)
        return TC_EBUSY;
      r->jobs[chosen] = (tm_job_t){0};
      r->jobs[chosen].owner_pid = p->graph.task[i].pid;
      r->jobs[chosen].scheduler.state[0].pid = p->graph.task[i].pid;
      r->jobs[chosen].scheduler.active_valid = 1;
      r->jobs[chosen].scheduler.active.count = 1;
      r->jobs[chosen].scheduler.active.task[0] = p->graph.task[i];
      r->jobs[chosen].scheduler.active.limits = p->limits.time;
      publish_stats(&r->jobs[chosen]);
    }
    r->mapping[next][i] = (uint8_t)chosen;
    used |= 1u << chosen;
  }
  r->plans[next] = *p;
  __atomic_store_n(&r->prepared_plan, next + 1, __ATOMIC_RELEASE);
  return TC_OK;
}
void tm_cancel_prepare(tm_runtime_t *r) {
  __atomic_store_n(&r->prepared_plan, 0, __ATOMIC_RELEASE);
}
int tm_commit(tm_runtime_t *r, int preserve) {
  if (!r || !tm_paused(r))
    return TC_EBUSY;
  unsigned next = __atomic_load_n(&r->prepared_plan, __ATOMIC_ACQUIRE);
  if (!next)
    return TC_EINVAL;
  ++r->generation;
  if (!preserve) {
    r->started = 0;
    r->previous_valid = 0;
    ++r->epoch_version;
  }
  __atomic_store_n(&r->active_plan, next - 1, __ATOMIC_RELEASE);
  r->valid = 1;
  __atomic_store_n(&r->prepared_plan, 0, __ATOMIC_RELEASE);
  return TC_OK;
}
int tm_install(tm_runtime_t *r, const tm_plan_t *p) {
  if (!r || !tm_paused(r))
    return TC_EBUSY;
  int rc = tm_prepare(r, p);
  return rc == TC_OK ? tm_commit(r, 0) : rc;
}
int tm_replace(tm_runtime_t *r, const tm_plan_t *p) {
  if (!r || !tm_paused(r))
    return TC_EBUSY;
  int rc = tm_prepare(r, p);
  return rc == TC_OK ? tm_commit(r, 1) : rc;
}
int tm_resume(tm_runtime_t *r) {
  if (!r || !r->valid || !tm_paused(r))
    return TC_EBUSY;
  uint32_t expected = 2;
  return __atomic_compare_exchange_n(&r->gate, &expected, 0, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)
             ? TC_OK
             : TC_EBUSY;
}
int tm_kick(tm_runtime_t *r, uint64_t frame, uint64_t release) {
  if (!r || !frame)
    return TC_EINVAL;
  uint32_t idle = 0;
  if (!__atomic_compare_exchange_n(&r->gate, &idle, 1, 0, __ATOMIC_SEQ_CST,
                                   __ATOMIC_RELAXED))
    return -1;
  int result = 1;
  if (!r->valid) {
    result = TC_EINVAL;
    goto done;
  }
  const tm_plan_t *p = plan(r);
  uint64_t ticks = p->limits.time.frame_ticks;
  if (release > UINT64_MAX - ticks || r->ops.clock(r->ops.ctx) < release ||
      (r->started &&
       (frame <= r->frame || frame - r->frame > UINT64_MAX / ticks ||
        release < r->release ||
        release - r->release != (frame - r->frame) * ticks))) {
    result = TC_ESTALE;
    goto done;
  }
  if (!tm_drained(r)) {
    __atomic_fetch_add(&r->skipped, 1u, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < TM_CORES; ++i)
      if (!aw_drained(r->worker[i]))
        __atomic_fetch_add(&r->core_late[i], 1u, __ATOMIC_RELAXED);
    result = 0;
    goto done;
  }
  for (unsigned i = 0; i < TM_CORES; ++i)
    if (r->worker[i]->stop || (r->worker[i]->publishing & 2u)) {
      result = TC_EBUSY;
      goto done;
    }
  if (!r->started)
    r->epoch = frame;
  r->previous_valid = r->started && frame == r->frame + 1;
  r->frame = frame;
  r->release = release;
  r->bank ^= 1;
  r->started = 1;
  __atomic_fetch_add(&r->accepted, 1u, __ATOMIC_RELAXED);
  uint32_t used = 0;
  for (unsigned i = 0; i < p->graph.count; ++i)
    used |= 1u << p->core[i];
  for (unsigned i = 0; i < TM_CORES; ++i)
    if (used & (1u << (i + 1)))
      (void)aw_kick_at(r->worker[i], frame, release);
done:
  __atomic_fetch_and(&r->gate, ~1u, __ATOMIC_RELEASE);
  return result;
}
