#include "audio_session.h"
#include "budget.h"
#include "el0_context.h"
#include "pmem.h"
#include "pmm.h"
#include "sample_bits.h"
#include "vmem.h"
#include <stddef.h>
_Static_assert(offsetof(session_params_t, events) == sizeof(param_queue_t),
               "queue storage layout");
static session_scene_t *active(audio_session_t *s) {
  return &s->scene[__atomic_load_n(&s->active, __ATOMIC_ACQUIRE)];
}
static int node_index(const session_scene_t *sc, uint32_t pid) {
  for (unsigned i = 0; i < sc->count; ++i)
    if (sc->nodes[i]->pid == pid)
      return (int)i;
  return -1;
}
static pm_slot_t *slot(session_node_t *n) {
  for (unsigned i = 0; i < PM_MAX_PLUGINS; ++i)
    if (n->manager->slots[i].used && n->manager->slots[i].pid == n->pid)
      return &n->manager->slots[i];
  return NULL;
}
static int management_guard(void *ctx) {
  audio_session_t *s = ctx;
  return __atomic_load_n(&s->editing, __ATOMIC_ACQUIRE) == el0_cpu_index() + 1;
}
static int scene_guard(void *ctx) {
  session_scene_t *sc = ctx;
  audio_session_t *s = sc->owner;
  return management_guard(s) &&
         (sc != active(s) || (!s->running && tm_paused(&s->runtime) &&
                              !__atomic_load_n(&s->readers, __ATOMIC_SEQ_CST)));
}
static void *edge_new(void *ctx) {
  session_scene_t *sc = ctx;
  for (unsigned i = 0; i < GRAPH_MAX_EDGES; ++i)
    if (!sc->rings[i]) {
      sc->rings[i] = 1;
      return &sc->rings[i];
    }
  return NULL;
}
static void edge_del(void *ctx, void *edge) {
  (void)ctx;
  *(uint32_t *)edge = 0;
}
static uint64_t clock_fn(void *ctx) {
  return ((audio_session_t *)ctx)->clock();
}
static long run_block(void *ctx) {
  session_node_t *n = ctx;
  uint32_t id, bits;
  for (unsigned i = 0;
       i < PM_PARAM_CAP && pq_pop(&n->parameters.queue, &id, &bits); ++i) {
    long rc = plugin_call_set_param_worker(n->plugin, id, bits);
    if (rc < 0)
      return rc;
  }
  return plugin_call_block(n->plugin, SESSION_IN_VA,
                           SESSION_IN_VA + n->frames * 4u, SESSION_OUT_VA,
                           SESSION_OUT_VA + n->frames * 4u, n->frames);
}
static long run_fn(void *ctx, uint32_t pid, uint64_t budget, uint64_t cutoff) {
  audio_session_t *s = ctx;
  session_scene_t *sc = active(s);
  int i = node_index(sc, pid);
  if (i < 0)
    return -1;
  uint64_t now = s->clock();
  if (now >= cutoff)
    return TC_RUN_DEADLINE;
  uint64_t limit = budget < cutoff - now ? budget : cutoff - now;
  long rc = budget_call(run_block, sc->nodes[i], limit);
  return rc == BUDGET_PREEMPTED && limit < budget ? TC_RUN_DEADLINE : rc;
}
static int prepare_fn(void *ctx, uint32_t pid, unsigned bank,
                      int previous_valid) {
  audio_session_t *s = ctx;
  session_scene_t *sc = active(s);
  int idx = node_index(sc, pid);
  if (idx < 0)
    return -1;
  session_node_t *n = sc->nodes[idx];
  const uint32_t *sources[GRAPH_MAX_EDGES];
  unsigned count = 0;
  int node = audio_graph_node_by_pid(&sc->graph.graph, pid);
  for (unsigned e = 0; e < GRAPH_MAX_EDGES; ++e) {
    const graph_edge_t *edge = &sc->graph.graph.edges[e];
    if (!edge->used || edge->dst != node)
      continue;
    int src = node_index(sc, sc->graph.graph.nodes[edge->src].pid);
    if (src < 0)
      return -1;
    if (edge->feedback && !previous_valid)
      continue;
    sources[count++] =
        sc->nodes[src]->output[edge->feedback ? (bank ^ 1) : bank];
  }
  for (unsigned j = 0; j < s->frames * 2; ++j) {
    uint32_t bits = 0;
    if (count == 1)
      bits = sources[0][j];
    else if (count) {
      int64_t mix = 0;
      for (unsigned k = 0; k < count; ++k)
        mix += sample_q31(sources[k][j]);
      bits = sample_float(mix);
    }
    n->input[j] = bits;
    n->scratch[j] = 0;
  }
  return 0;
}
static void output_fn(void *ctx, uint32_t pid, unsigned bank,
                      enum tc_output action) {
  audio_session_t *s = ctx;
  session_scene_t *sc = active(s);
  int i = node_index(sc, pid);
  if (i < 0)
    return;
  session_node_t *n = sc->nodes[i];
  for (unsigned j = 0; j < s->frames * 2; ++j)
    n->output[bank][j] = action == TC_OUTPUT_OK       ? n->scratch[j]
                         : action == TC_OUTPUT_BYPASS ? n->input[j]
                                                      : 0;
}
static void kill_fn(void *ctx, uint32_t pid) {
  audio_session_t *s = ctx;
  session_scene_t *sc = active(s);
  int i = node_index(sc, pid);
  if (i >= 0)
    process_kill(sc->nodes[i]->plugin->proc, BUDGET_PREEMPTED);
}
static void scene_init(audio_session_t *s, session_scene_t *sc) {
  *sc = (session_scene_t){0};
  sc->owner = s;
  sc->cores = s->available_mask == 14 ? 3 : s->available_mask == 6 ? 2 : 1;
  gc_ring_ops_t ops = {.ring_new = edge_new, .ring_del = edge_del, .ctx = sc};
  gc_init(&sc->graph, &ops);
  gc_add_dac(&sc->graph);
  gc_set_mutation_guard(&sc->graph, scene_guard, sc);
}
int session_init(audio_session_t *s, audio_worker_t *workers[TM_CORES],
                 const tm_limits_t *limits, uint64_t (*clock)(void),
                 uint64_t hz, uint32_t rate, uint32_t frames, uint64_t life,
                 uint32_t available) {
  if (!s || !limits || !clock || !hz || hz > UINT32_MAX || !rate || !frames ||
      frames > SESSION_MAX_FRAMES || !life || life > INT64_MAX ||
      (available != 2 && available != 6 && available != 14))
    return TC_EINVAL;
  *s = (audio_session_t){0};
  s->limits = *limits;
  s->clock = clock;
  s->counter_hz = hz;
  s->sample_rate = rate;
  s->frames = frames;
  s->lifecycle_ticks = life;
  s->timeout_ticks = hz;
  s->available_mask = available;
  tm_ops_t ops = {clock_fn, run_fn, prepare_fn, output_fn, kill_fn, s};
  int rc = tm_init(&s->runtime, workers, &ops);
  if (rc != TC_OK)
    return rc;
  for (unsigned i = 0; i < 2; ++i) {
    gc_ring_ops_t no_edges = {0};
    gc_init(&s->lifecycle_graph[i], &no_edges);
    gc_set_mutation_guard(&s->lifecycle_graph[i], management_guard, s);
    pm_init(&s->manager[i], &s->lifecycle_graph[i]);
    pm_set_lifecycle_budget(&s->manager[i], life);
    scene_init(s, &s->scene[i]);
  }
  return TC_OK;
}
int session_add_blob(audio_session_t *s, const char *name, const void *bytes,
                     uint32_t len) {
  if (!s || s->running || !name || !bytes || !len)
    return TC_EINVAL;
  int rc = pm_register_blob(&s->manager[0], name, (void *)bytes, len);
  return rc ? rc : pm_register_blob(&s->manager[1], name, (void *)bytes, len);
}
void session_mount(audio_session_t *s, fat_fs_t *fat) {
  if (s && !s->running)
    for (unsigned i = 0; i < 2; ++i)
      pm_mount_sd(&s->manager[i], fat);
}
static int dispose(audio_session_t *s, session_node_t *n) {
  if (!n->used)
    return TC_OK;
  plugin_t *pl = n->plugin;
  __atomic_store_n(&pl->host_refs, 0, __ATOMIC_RELEASE);
  pl->bound_cpu = 0;
  if (pl->proc->state != PROC_KILLED) {
    long rc = plugin_call_destroy(pl);
    if (rc < 0)
      s->last_lifecycle_result = rc;
  }
  int rc = pm_unload(n->manager, n->pid);
  if (rc == PM_OK)
    *n = (session_node_t){0};
  return rc;
}
static int reap(audio_session_t *s, session_scene_t *old,
                const session_scene_t *keep) {
  for (unsigned i = 0; i < old->count; ++i) {
    session_node_t *n = old->nodes[i];
    if (node_index(keep, n->pid) < 0) {
      int rc = dispose(s, n);
      if (rc != TC_OK)
        return rc;
    }
  }
  old->count = 0;
  return TC_OK;
}
static int lock(audio_session_t *s) {
  if (!s)
    return TC_EINVAL;
  uint32_t empty = 0;
  if (!__atomic_compare_exchange_n(&s->editing, &empty, el0_cpu_index() + 1, 0,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return TC_EBUSY;
  if (__atomic_load_n(&s->pending, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&s->editing, 0, __ATOMIC_RELEASE);
    return TC_EBUSY;
  }
  if (s->retire) {
    int rc = reap(s, &s->scene[s->retire - 1], active(s));
    if (rc != TC_OK) {
      __atomic_store_n(&s->editing, 0, __ATOMIC_RELEASE);
      return rc;
    }
    s->retire = 0;
  }
  return TC_OK;
}
static void unlock(audio_session_t *s) {
  __atomic_store_n(&s->editing, 0, __ATOMIC_RELEASE);
}
static int pause_locked(audio_session_t *s) {
  tm_request_pause(&s->runtime);
  uint64_t start = s->clock();
  while (!tm_paused(&s->runtime) ||
         __atomic_load_n(&s->readers, __ATOMIC_SEQ_CST))
    if (s->clock() - start >= s->timeout_ticks)
      return SESSION_ETIMEOUT;
  __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
  return TC_OK;
}
static int admit(audio_session_t *s, session_scene_t *sc) {
  uint32_t mask = (1u << (sc->cores + 1)) - 2u;
  if (mask & ~s->available_mask)
    return TC_EINVAL;
  return tm_plan_build(&sc->graph.graph, sc->contracts, sc->count, &s->limits,
                       mask, sc->pins, &sc->plan, &s->admission);
}
static session_scene_t *clone(audio_session_t *s) {
  session_scene_t *src = active(s),
                  *dst = &s->scene[(unsigned)(src - s->scene) ^ 1u];
  *dst = *src;
  dst->graph.ops.ctx = dst;
  dst->graph.mutation_guard_ctx = dst;
  for (unsigned i = 0; i < GRAPH_MAX_EDGES; ++i) {
    dst->rings[i] = dst->graph.graph.edges[i].used ? 1u : 0u;
    if (dst->graph.graph.edges[i].used)
      dst->graph.graph.edges[i].ring = &dst->rings[i];
  }
  return dst;
}
static int install_scene(audio_session_t *s, unsigned index, int live) {
  session_scene_t *sc = &s->scene[index];
  int rc =
      live ? tm_commit(&s->runtime, 1) : tm_install(&s->runtime, &sc->plan);
  if (rc != TC_OK)
    return rc;
  for (unsigned i = 0; i < sc->count; ++i)
    sc->nodes[i]->plugin->bound_cpu = sc->plan.core[i];
  __atomic_store_n(&s->active, index, __ATOMIC_RELEASE);
  if (!live)
    s->collected_frame = 0;
  return TC_OK;
}
static int commit(audio_session_t *s, session_scene_t *candidate) {
  int rc = admit(s, candidate);
  session_scene_t *old = active(s);
  if (rc != TC_OK) {
    (void)reap(s, candidate, old);
    return rc;
  }
  unsigned index = (unsigned)(candidate - s->scene);
  if (!s->running) {
    rc = install_scene(s, index, 0);
    if (rc == TC_OK)
      rc = reap(s, old, candidate);
    return rc;
  }
  rc = tm_prepare(&s->runtime, &candidate->plan);
  if (rc != TC_OK) {
    (void)reap(s, candidate, old);
    return rc;
  }
  s->retire = (unsigned)(old - s->scene) + 1;
  s->swap_result = TC_OK;
  uint32_t ready = index + 1;
  __atomic_store_n(&s->pending, ready, __ATOMIC_RELEASE);
  uint64_t start = s->clock();
  while (__atomic_load_n(&s->pending, __ATOMIC_ACQUIRE)) {
    if (s->clock() - start < s->timeout_ticks)
      continue;
    uint32_t expected = ready;
    if (__atomic_compare_exchange_n(&s->pending, &expected, 0, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      tm_cancel_prepare(&s->runtime);
      s->retire = 0;
      (void)reap(s, candidate, old);
      return SESSION_ETIMEOUT;
    }
    /* A claimed swap cannot safely be cancelled or freed. Next control
     * access reaps after its acknowledgement; expose indeterminate status. */
    return SESSION_EINPROGRESS;
  }
  if (s->swap_result != TC_OK) {
    s->retire = 0;
    return s->swap_result;
  }
  rc = reap(s, old, active(s));
  s->retire = 0;
  return rc;
}
int session_pause(audio_session_t *s) {
  int rc = lock(s);
  if (rc != TC_OK)
    return rc;
  rc = pause_locked(s);
  unlock(s);
  return rc;
}
int session_start(audio_session_t *s) {
  int rc = lock(s);
  if (rc != TC_OK)
    return rc;
  if (!s->running) {
    session_scene_t *sc = active(s);
    rc = sc->count ? admit(s, sc) : TC_ENODEV;
    if (rc == TC_OK)
      rc = install_scene(s, (unsigned)(sc - s->scene), 0);
    if (rc == TC_OK) {
      rc = tm_resume(&s->runtime);
      if (rc == TC_OK)
        __atomic_store_n(&s->running, 1, __ATOMIC_RELEASE);
    }
  }
  unlock(s);
  return rc;
}

static int map_io(audio_session_t *s, session_node_t *n) {
  uintptr_t input = phys_alloc_page_zero();
  if (!input)
    return SESSION_EIO;
  if (plugin_map_region(n->plugin, SESSION_IN_VA, input, PAGE_SIZE, VMM_READ)) {
    phys_free_page(input);
    return SESSION_EIO;
  }
  uintptr_t output = phys_alloc_page_zero();
  if (!output)
    return SESSION_EIO;
  if (plugin_map_region(n->plugin, SESSION_OUT_VA, output, PAGE_SIZE,
                        VMM_READ | VMM_WRITE)) {
    phys_free_page(output);
    return SESSION_EIO;
  }
  n->input = P2V(input);
  n->scratch = P2V(output);
  n->frames = s->frames;
  pq_init(&n->parameters.queue, PM_PARAM_CAP);
  return TC_OK;
}
static long scene_load(audio_session_t *s, session_scene_t *sc,
                       const char *path, const temporal_contract_t *c) {
  if (sc->count >= TC_MAX_TASKS - 1)
    return SESSION_EFULL;
  session_node_t *n = NULL;
  for (unsigned i = 0; i < SESSION_INSTANCES; ++i)
    if (!s->instances[i].used) {
      n = &s->instances[i];
      break;
    }
  if (!n)
    return SESSION_EFULL;
  plugin_mgr_t *m = NULL;
  for (unsigned k = 0; k < 2 && !m; ++k)
    for (unsigned j = 0; j < PM_MAX_PLUGINS; ++j)
      if (!s->manager[k].slots[j].used) {
        m = &s->manager[k];
        break;
      }
  if (!m)
    return SESSION_EFULL;
  long pid = pm_load(m, path);
  if (pid <= 0) {
    s->last_lifecycle_result = pid;
    return SESSION_EPLUGIN;
  }
  *n = (session_node_t){.pid = (uint32_t)pid,
                        .used = 1,
                        .manager = m,
                        .plugin = pm_plugin(m, (uint32_t)pid)};
  int rc = map_io(s, n);
  if (rc == TC_OK) {
    s->last_lifecycle_result =
        plugin_call_init(n->plugin, s->sample_rate, s->frames);
    if (s->last_lifecycle_result != 0)
      rc = SESSION_EPLUGIN;
  }
  if (rc != TC_OK) {
    (void)dispose(s, n);
    return rc;
  }
  if (gc_add_plugin(&sc->graph, (uint32_t)pid) < 0) {
    (void)dispose(s, n);
    return SESSION_EFULL;
  }
  unsigned i = sc->count++;
  sc->nodes[i] = n;
  sc->contracts[i] = *c;
  sc->contracts[i].pid = (uint32_t)pid;
  sc->pins[i] = 0;
  __atomic_store_n(&n->plugin->host_refs, 1, __ATOMIC_RELEASE);
  return pid;
}
long session_load(audio_session_t *s, const char *path,
                  const temporal_contract_t *c) {
  if (!path || !c)
    return TC_EINVAL;
  int rc = lock(s);
  if (rc != TC_OK)
    return rc;
  session_scene_t *candidate = clone(s);
  long pid = scene_load(s, candidate, path, c);
  if (pid > 0)
    rc = commit(s, candidate);
  else {
    (void)reap(s, candidate, active(s));
    rc = (int)pid;
  }
  unlock(s);
  return rc == TC_OK ? pid : rc;
}
static void remove_node(session_scene_t *sc, unsigned i) {
  uint32_t pid = sc->nodes[i]->pid;
  int idx = audio_graph_node_by_pid(&sc->graph.graph, pid);
  for (unsigned e = 0; e < GRAPH_MAX_EDGES; ++e) {
    graph_edge_t *edge = &sc->graph.graph.edges[e];
    if (!edge->used)
      continue;
    if (edge->src == idx || edge->dst == idx)
      gc_disconnect(&sc->graph, sc->graph.graph.nodes[edge->src].pid,
                    sc->graph.graph.nodes[edge->dst].pid);
  }
  audio_graph_remove_node(&sc->graph.graph, idx);
  for (unsigned j = i + 1; j < sc->count; ++j) {
    sc->nodes[j - 1] = sc->nodes[j];
    sc->contracts[j - 1] = sc->contracts[j];
    sc->pins[j - 1] = sc->pins[j];
  }
  --sc->count;
}
int session_unload(audio_session_t *s, uint32_t pid) {
  int rc = lock(s);
  if (rc != TC_OK)
    return rc;
  session_scene_t *candidate = clone(s);
  int i = node_index(candidate, pid);
  if (i < 0)
    rc = TC_ENODEV;
  else {
    remove_node(candidate, (unsigned)i);
    rc = commit(s, candidate);
  }
  unlock(s);
  return rc;
}
static int wire(audio_session_t *s, uint32_t src, uint32_t dst, int mode) {
  int rc = lock(s);
  if (rc != TC_OK)
    return rc;
  session_scene_t *candidate = clone(s);
  rc = mode == 2 ? gc_disconnect(&candidate->graph, src, dst)
       : mode    ? gc_connect_feedback(&candidate->graph, src, dst)
                 : gc_connect(&candidate->graph, src, dst);
  if (rc == TC_OK)
    rc = commit(s, candidate);
  unlock(s);
  return rc;
}
int session_wire(audio_session_t *s, uint32_t src, uint32_t dst, int feedback) {
  return feedback < 0 || feedback > 1 || (feedback && dst == 0)
             ? TC_EINVAL
             : wire(s, src, dst, feedback);
}
int session_unwire(audio_session_t *s, uint32_t src, uint32_t dst) {
  return wire(s, src, dst, 2);
}
static int change(audio_session_t *s, uint32_t pid,
                  const temporal_contract_t *c, int pin, unsigned cores) {
  int rc = lock(s);
  if (rc != TC_OK)
    return rc;
  session_scene_t *candidate = clone(s);
  int i = node_index(candidate, pid);
  if (cores)
    candidate->cores = cores;
  else if (i < 0)
    rc = TC_ENODEV;
  else {
    if (c)
      candidate->contracts[i] = *c;
    if (pin >= 0)
      candidate->pins[i] = (uint8_t)pin;
  }
  if (rc == TC_OK)
    rc = commit(s, candidate);
  unlock(s);
  return rc;
}
int session_set_contract(audio_session_t *s, const temporal_contract_t *c) {
  return c ? change(s, c->pid, c, -1, 0) : TC_EINVAL;
}
int session_set_cores(audio_session_t *s, unsigned cores) {
  return cores < 1 || cores > 3 ? TC_EINVAL : change(s, 0, NULL, -1, cores);
}
int session_pin(audio_session_t *s, uint32_t pid, unsigned core) {
  return core > 3 ? TC_EINVAL : change(s, pid, NULL, (int)core, 0);
}
static int record_param(session_node_t *n, uint32_t id, uint32_t bits,
                        int apply) {
  pm_slot_t *p = slot(n);
  if (!p)
    return TC_ENODEV;
  unsigned k = 0;
  while (k < (unsigned)p->n_params && p->params[k].id != id)
    ++k;
  if (k >= PM_SLOT_PARAMS)
    return SESSION_EFULL;
  if (apply) {
    __atomic_store_n(&n->plugin->host_refs, 0, __ATOMIC_RELEASE);
    long rc = plugin_call_set_param(n->plugin, id, bits);
    __atomic_store_n(&n->plugin->host_refs, 1, __ATOMIC_RELEASE);
    if (rc < 0)
      return SESSION_EPLUGIN;
  } else if (!pq_push(&n->parameters.queue, id, bits))
    return SESSION_EFULL;
  p->params[k].id = id;
  p->params[k].bits = bits;
  if (k == (unsigned)p->n_params)
    ++p->n_params;
  return TC_OK;
}
int session_set_param(audio_session_t *s, uint32_t pid, uint32_t id,
                      uint32_t bits) {
  int rc = lock(s);
  if (rc != TC_OK)
    return rc;
  session_scene_t *sc = active(s);
  int i = node_index(sc, pid);
  rc = i < 0 ? TC_ENODEV
             : record_param(sc->nodes[i], id, bits,
                            !s->running && tm_paused(&s->runtime));
  unlock(s);
  return rc;
}
int session_clear(audio_session_t *s) {
  int rc = lock(s);
  if (rc != TC_OK)
    return rc;
  session_scene_t *candidate = &s->scene[(unsigned)(active(s) - s->scene) ^ 1u];
  scene_init(s, candidate);
  rc = commit(s, candidate);
  unlock(s);
  return rc;
}
int session_collect(audio_session_t *s, int16_t *out) {
  if (!s || !out)
    return 0;
  __atomic_fetch_add(&s->readers, 1, __ATOMIC_SEQ_CST);
  int result = 0;
  for (unsigned i = 0; i < s->frames * 2; ++i)
    out[i] = 0;
  if (__atomic_load_n(&s->runtime.gate, __ATOMIC_SEQ_CST) & 2u) {
    __atomic_fetch_add(&s->intentional_silence, 1, __ATOMIC_RELAXED);
    goto done;
  }
  if (!s->runtime.started || !tm_drained(&s->runtime) ||
      s->runtime.frame == s->collected_frame) {
    __atomic_fetch_add(&s->unavailable, 1, __ATOMIC_RELAXED);
    goto done;
  }
  session_scene_t *sc = active(s);
  unsigned bank = s->runtime.bank;
  const uint32_t *sources[GRAPH_MAX_EDGES];
  unsigned count = 0;
  for (unsigned e = 0; e < GRAPH_MAX_EDGES; ++e) {
    const graph_edge_t *edge = &sc->graph.graph.edges[e];
    if (!edge->used || edge->dst != sc->graph.graph.dac_node)
      continue;
    int i = node_index(sc, sc->graph.graph.nodes[edge->src].pid);
    if (i >= 0)
      sources[count++] = sc->nodes[i]->output[bank];
  }
  for (unsigned j = 0; j < s->frames; ++j)
    for (unsigned ch = 0; ch < 2; ++ch) {
      int64_t sum = 0;
      for (unsigned i = 0; i < count; ++i)
        sum += sample_q31(sources[i][j + ch * s->frames]);
      if (sum > INT32_MAX)
        sum = INT32_MAX;
      if (sum < INT32_MIN)
        sum = INT32_MIN;
      out[j * 2 + ch] = (int16_t)(sum / 65536);
    }
  s->collected_frame = s->runtime.frame;
  result = 1;
done:
  __atomic_fetch_sub(&s->readers, 1, __ATOMIC_SEQ_CST);
  return result;
}
int session_tick(audio_session_t *s, uint64_t frame, uint64_t release,
                 int16_t *out) {
  int got = session_collect(s, out);
  uint32_t ready = __atomic_load_n(&s->pending, __ATOMIC_ACQUIRE);
  if (ready && ready <= 2 && tm_drained(&s->runtime)) {
    uint32_t expected = ready;
    if (__atomic_compare_exchange_n(&s->pending, &expected, ready | 4u, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      tm_request_pause(&s->runtime);
      int rc = install_scene(s, ready - 1, 1);
      if (rc == TC_OK)
        rc = tm_resume(&s->runtime);
      if (rc != TC_OK)
        __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
      s->swap_result = rc;
      __atomic_fetch_add(&s->swaps, 1, __ATOMIC_RELAXED);
      __atomic_store_n(&s->pending, 0, __ATOMIC_RELEASE);
    }
  }
  if (__atomic_load_n(&s->running, __ATOMIC_ACQUIRE)) {
    int rc = tm_kick(&s->runtime, frame, release);
    __atomic_store_n(&s->last_kick_result, rc, __ATOMIC_RELAXED);
    if (rc < -1)
      __atomic_fetch_add(&s->invalid_kicks, 1u, __ATOMIC_RELAXED);
  }
  return got;
}
static int capture_locked(audio_session_t *s, session_preset_t *out) {
  session_scene_t *sc = active(s);
  session_preset_t p = {0};
  p.sample_rate = s->sample_rate;
  p.frames = s->frames;
  p.counter_hz = s->counter_hz;
  p.cores = sc->cores;
  for (unsigned i = 0; i < sc->count; ++i) {
    pm_slot_t *pm = slot(sc->nodes[i]);
    if (!pm)
      return TC_ENODEV;
    int idx = patch_add_plugin(&p.patch, pm->path);
    if (idx < 0)
      return SESSION_EFULL;
    p.contract[i] = sc->contracts[i];
    p.contract[i].pid = i + 1;
    p.pin[i] = sc->pins[i];
    for (int k = 0; k < pm->n_params; ++k)
      if (patch_add_param(&p.patch, idx, pm->params[k].id, pm->params[k].bits) <
          0)
        return SESSION_EFULL;
  }
  for (unsigned e = 0; e < GRAPH_MAX_EDGES; ++e) {
    const graph_edge_t *edge = &sc->graph.graph.edges[e];
    if (!edge->used)
      continue;
    int a = node_index(sc, sc->graph.graph.nodes[edge->src].pid);
    int b = edge->dst == sc->graph.graph.dac_node
                ? PATCH_DAC
                : node_index(sc, sc->graph.graph.nodes[edge->dst].pid);
    int k = patch_add_edge(&p.patch, a, b);
    if (k < 0)
      return SESSION_EFORMAT;
    p.feedback[k] = (uint8_t)edge->feedback;
  }
  *out = p;
  return TC_OK;
}
int session_capture(audio_session_t *s, session_preset_t *out) {
  if (!out)
    return TC_EINVAL;
  int rc = lock(s);
  if (rc != TC_OK)
    return rc;
  rc = capture_locked(s, out);
  unlock(s);
  return rc;
}
static int same(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}
int session_save(audio_session_t *s, const char *path) {
  if (!path || !*path || same(path, "/sd/TESS_TMP.TSP") ||
      same(path, "/rd/TESS_TMP.TSP"))
    return TC_EINVAL;
  int rc = lock(s);
  if (rc != TC_OK)
    return rc;
  session_preset_t p;
  rc = capture_locked(s, &p);
  const char *tmp =
      path[0] == '/' && path[1] == 's' && path[2] == 'd' && path[3] == '/'
          ? "/sd/TESS_TMP.TSP"
          : "/rd/TESS_TMP.TSP";
  if (rc == TC_OK) {
    long n = session_preset_write(&p, s->text, sizeof(s->text));
    if (n < 0)
      rc = (int)n;
    else {
      rc = vfs_write(&s->manager[0].vfs, tmp, (const uint8_t *)s->text,
                     (uint32_t)n);
      if (rc == 0)
        rc = vfs_rename(&s->manager[0].vfs, tmp, path);
    }
  }
  unlock(s);
  return rc;
}
int session_restore(audio_session_t *s, const char *path) {
  if (!path)
    return TC_EINVAL;
  int rc = lock(s);
  if (rc != TC_OK)
    return rc;
  const uint8_t *data = NULL;
  long len = vfs_resolve(&s->manager[0].vfs, path, &data, (uint8_t *)s->text,
                         sizeof(s->text));
  session_preset_t model;
  tm_plan_t test;
  rc = len <= 0
           ? SESSION_EIO
           : session_preset_parse((const char *)data, (uint32_t)len, &model);
  if (rc == TC_OK &&
      (model.counter_hz != s->counter_hz ||
       model.sample_rate != s->sample_rate || model.frames != s->frames))
    rc = TC_EINVAL;
  if (rc == TC_OK)
    rc = session_preset_plan(&model, &s->limits, &test, &s->admission);
  if (rc != TC_OK) {
    unlock(s);
    return rc;
  }
  session_scene_t *candidate = &s->scene[(unsigned)(active(s) - s->scene) ^ 1u];
  scene_init(s, candidate);
  candidate->cores = model.cores;
  for (int i = 0; rc == TC_OK && i < model.patch.n_plugins; ++i) {
    long pid = scene_load(s, candidate, model.patch.plugins[i].path,
                          &model.contract[i]);
    if (pid <= 0)
      rc = (int)pid;
    else
      candidate->pins[i] = model.pin[i];
  }
  for (int i = 0; rc == TC_OK && i < model.patch.n_edges; ++i) {
    patch_edge_t *e = &model.patch.edges[i];
    uint32_t a = candidate->nodes[e->src]->pid,
             b = e->dst == PATCH_DAC ? 0 : candidate->nodes[e->dst]->pid;
    rc = model.feedback[i] ? gc_connect_feedback(&candidate->graph, a, b)
                           : gc_connect(&candidate->graph, a, b);
  }
  for (int i = 0; rc == TC_OK && i < model.patch.n_params; ++i) {
    patch_param_t *v = &model.patch.params[i];
    rc = record_param(candidate->nodes[v->plugin], v->id, v->bits, 1);
  }
  if (rc == TC_OK)
    rc = commit(s, candidate);
  else
    (void)reap(s, candidate, active(s));
  unlock(s);
  return rc;
}
