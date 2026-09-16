#include "temporal_multicore.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
static unsigned checks;
#define CHECK(x)                                                               \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(x)) {                                                                \
      fprintf(stderr, "TM FAIL %d: %s\n", __LINE__, #x);                       \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static temporal_contract_t task(unsigned pid, uint64_t budget) {
  return (temporal_contract_t){.pid = pid,
                               .period = 1000,
                               .deadline = 950,
                               .cpu_budget = budget,
                               .criticality = TC_HARD,
                               .overrun_policy = TC_MUTE_THEN_KILL,
                               .kill_after = 3};
}
static void graph(audio_graph_t *g, unsigned n) {
  audio_graph_init(g, NULL);
  for (unsigned i = 0; i < n; ++i)
    CHECK(audio_graph_add_node(g, i + 1) >= 0);
}
static void admission(void) {
  audio_graph_t g;
  graph(&g, 4);
  temporal_contract_t c[] = {task(1, 450), task(2, 450), task(3, 450),
                             task(4, 100)};
  tm_limits_t limits = {{1000, 10, 10}, 10};
  tm_plan_t p, old;
  CHECK(tm_plan_build(&g, c, 4, &limits, 2, NULL, &p, NULL) == TC_EADMISSION);
  CHECK(tm_plan_build(&g, c, 4, &limits, 14, NULL, &p, NULL) == TC_OK);
  CHECK(p.core[0] == 1 && p.core[1] == 2 && p.core[2] == 3);
  old = p;
  CHECK(tm_plan_build(&g, c, 4, &limits, 1, NULL, &p, NULL) == TC_EINVAL &&
        !memcmp(&old, &p, sizeof p));
  CHECK(audio_graph_connect(&g, 0, 3) >= 0);
  uint8_t pins[] = {1, 2, 3, 2};
  CHECK(tm_plan_build(&g, c, 4, &limits, 14, pins, &p, NULL) == TC_OK);
  CHECK(p.cross_edges == 1 && p.start[3] >= p.finish[0] + 10);
  CHECK(p.latest_finish[0] + 10 <= p.latest_finish[3] - 120);
  old = p;
  c[3].deadline = 500;
  CHECK(tm_plan_build(&g, c, 4, &limits, 14, pins, &p, NULL) == TC_EADMISSION &&
        !memcmp(&old, &p, sizeof p));
  c[3] = task(4, 100);
  c[0].criticality = TC_BEST_EFFORT;
  CHECK(tm_plan_build(&g, c, 4, &limits, 14, pins, &p, NULL) == TC_EDEPENDENCY);
  c[0].criticality = TC_HARD;
  c[0].period = 2000;
  CHECK(tm_plan_build(&g, c, 4, &limits, 14, pins, &p, NULL) == TC_EDEPENDENCY);
  c[0].period = 1000;
  c[1].criticality = TC_BEST_EFFORT;
  c[1].cpu_budget = 900;
  CHECK(tm_plan_build(&g, c, 4, &limits, 14, pins, &p, NULL) == TC_OK);
  pins[3] = 4;
  CHECK(tm_plan_build(&g, c, 4, &limits, 14, pins, &p, NULL) == TC_EINVAL);
}
static uint32_t rng = 0x1354abdu;
static unsigned rnd(void) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}
static void properties(void) {
  unsigned accepted = 0, rejected = 0;
  for (unsigned trial = 0; trial < 5000; ++trial) {
    unsigned n = 1 + rnd() % 12;
    audio_graph_t g;
    graph(&g, n);
    temporal_contract_t c[12];
    uint8_t pins[12];
    for (unsigned i = 0; i < n; ++i) {
      c[i] = task(i + 1, 10 + rnd() % 500);
      c[i].deadline = c[i].cpu_budget + 20 + rnd() % 400;
      c[i].period = 1000 * (1 + rnd() % 4);
      c[i].criticality = rnd() % 3;
      pins[i] = rnd() % 4;
      if (i) {
        unsigned parent = rnd() % i;
        if (c[i].period % c[parent].period == 0 &&
            c[parent].criticality <= c[i].criticality)
          CHECK(audio_graph_connect(&g, parent, i) >= 0);
      }
    }
    tm_limits_t lim = {{1000, 10, 10}, 15};
    tm_plan_t p;
    int rc = tm_plan_build(&g, c, n, &lim, 14, pins, &p, NULL);
    if (rc != TC_OK) {
      CHECK(rc == TC_EADMISSION);
      ++rejected;
      continue;
    }
    ++accepted;
    uint64_t last[4] = {0};
    for (unsigned k = 0; k < n; ++k) {
      unsigned i = p.graph.order[k], cpu = p.core[i];
      uint64_t cost =
          20 + (c[i].criticality == TC_BEST_EFFORT ? 0 : c[i].cpu_budget);
      CHECK(cpu >= 1 && cpu <= 3 && (!pins[i] || pins[i] == cpu));
      CHECK(p.start[i] >= last[cpu] && p.finish[i] == p.start[i] + cost);
      CHECK(p.finish[i] <= p.latest_finish[i] &&
            p.latest_finish[i] <= c[i].deadline);
      CHECK(p.latest_finish[i] >= cost);
      for (unsigned j = 0; j < n; ++j)
        if (p.graph.deps[i] & (1u << j)) {
          unsigned gap = p.core[j] == cpu ? 0 : 15;
          CHECK(p.start[i] >= p.finish[j] + gap);
          CHECK(p.latest_finish[j] + gap <= p.latest_finish[i] - cost);
        }
      last[cpu] = p.finish[i];
    }
  }
  CHECK(accepted > 100 && rejected > 100);
  printf("TM generated admission: %u accepted / %u rejected\n", accepted,
         rejected);
}
static tm_runtime_t runtime;
static audio_worker_t workers[3];
static uint64_t fake_time;
static uint64_t samples[4];
static uint32_t bad, stop;
static uint64_t clock_fn(void *ctx) {
  (void)ctx;
  thrd_yield();
  return __atomic_add_fetch(&fake_time, 1, __ATOMIC_RELAXED);
}
static int prepare_fn(void *ctx, uint32_t pid, unsigned bank, int previous) {
  (void)ctx;
  (void)bank;
  (void)previous;
  if (pid == 1 && samples[1] != runtime.frame)
    __atomic_store_n(&bad, 1, __ATOMIC_RELAXED);
  return 0;
}
static long run_fn(void *ctx, uint32_t pid, uint64_t budget, uint64_t cutoff) {
  (void)ctx;
  (void)pid;
  (void)budget;
  (void)cutoff;
  __atomic_fetch_add(&fake_time, 10, __ATOMIC_RELAXED);
  return 0;
}
static void output_fn(void *ctx, uint32_t pid, unsigned bank,
                      enum tc_output action) {
  (void)ctx;
  (void)bank;
  samples[pid - 1] = runtime.frame;
  if (action != TC_OUTPUT_OK)
    __atomic_store_n(&bad, 1, __ATOMIC_RELAXED);
}
static void kill_fn(void *ctx, uint32_t pid) {
  (void)ctx;
  (void)pid;
  __atomic_store_n(&bad, 1, __ATOMIC_RELAXED);
}
static void *worker_thread(void *ctx) {
  audio_worker_t *w = ctx;
  while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
    if (!aw_worker_step(w))
      thrd_yield();
  }
  return NULL;
}
static void execution(void) {
  audio_graph_t g;
  graph(&g, 3);
  CHECK(audio_graph_connect(&g, 1, 0) >= 0);
  temporal_contract_t c[] = {task(1, 100), task(2, 100), task(3, 100)};
  for (unsigned i = 0; i < 3; ++i) {
    c[i].period = 1000000;
    c[i].deadline = 950000;
  }
  uint8_t pins[] = {1, 2, 3};
  tm_limits_t limits = {{1000000, 1000, 1000}, 1000};
  tm_plan_t p;
  CHECK(tm_plan_build(&g, c, 3, &limits, 14, pins, &p, NULL) == TC_OK);
  audio_worker_t *wp[3];
  pthread_t threads[3];
  for (unsigned i = 0; i < 3; ++i) {
    aw_init(&workers[i], i + 1);
    wp[i] = &workers[i];
  }
  tm_ops_t ops = {clock_fn, run_fn, prepare_fn, output_fn, kill_fn, NULL};
  CHECK(tm_init(&runtime, wp, &ops) == TC_OK && tm_paused(&runtime));
  CHECK(tm_install(&runtime, &p) == TC_OK && tm_resume(&runtime) == TC_OK);
  for (unsigned i = 0; i < 3; ++i)
    CHECK(!pthread_create(&threads[i], NULL, worker_thread, wp[i]));
  for (unsigned seq = 1; seq <= 1000; ++seq) {
    __atomic_store_n(&fake_time, (uint64_t)seq * 1000000 + 1000,
                     __ATOMIC_RELAXED);
    CHECK(tm_kick(&runtime, seq, (uint64_t)seq * 1000000) == 1);
    while (!tm_drained(&runtime))
      thrd_yield();
    tc_task_state_t stat;
    CHECK(tm_snapshot(&runtime, 1, &stat) == TC_OK && stat.completed == seq);
    tm_request_pause(&runtime);
    CHECK(tm_paused(&runtime));
    CHECK(tm_kick(&runtime, seq + 1, (uint64_t)(seq + 1) * 1000000) == -1);
    CHECK(tm_resume(&runtime) == TC_OK);
  }
  __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
  for (unsigned i = 0; i < 3; ++i)
    CHECK(!pthread_join(threads[i], NULL));
  CHECK(!bad && runtime.accepted == 1000 && !runtime.skipped);
  tm_request_pause(&runtime);
  CHECK(tm_paused(&runtime));
  /* With consumer CPU1 run before its producer, the bound expires and it
   * must publish silence, not read the previous block's sample storage. */
  CHECK(tm_install(&runtime, &p) == TC_OK && tm_resume(&runtime) == TC_OK);
  fake_time = 2000000000ull + 1000;
  CHECK(tm_kick(&runtime, 2000, 2000000000ull) == 1);
  fake_time = 2000950000ull;
  CHECK(aw_worker_step(&workers[0]) == 1);
  tc_task_state_t s;
  CHECK(tm_snapshot(&runtime, 1, &s) == TC_OK && s.deadline_misses == 1 &&
        s.budget_overruns == 0);
}
int main(void) {
  admission();
  properties();
  execution();
  printf("MULTICORE TEMPORAL HOST: PASS (%u checks)\n", checks);
  return 0;
}
