/* M11: admitted, dependency-aware execution on CPU1..3. */
#ifndef ARM64_TEMPORAL_MULTICORE_H
#define ARM64_TEMPORAL_MULTICORE_H
#include "audio_worker.h"
#include "temporal.h"
#define TM_CORES 3u

typedef struct {
  tc_limits_t time;
  uint64_t handoff_ticks;
} tm_limits_t;
typedef struct {
  tc_plan_t graph;
  tm_limits_t limits;
  uint32_t core_mask;
  uint8_t core[TC_MAX_TASKS]; /* hardware CPU 1..3, never CPU0 */
  uint64_t start[TC_MAX_TASKS], finish[TC_MAX_TASKS];
  uint64_t latest_finish[TC_MAX_TASKS], core_reserved[TM_CORES];
  uint32_t cross_edges;
} tm_plan_t;
/* Sufficient, conservative list admission, not an optimal bin-packing solver.
 * pin[i]=0 means automatic; otherwise a hardware CPU 1..3 in core_mask.
 * Reserve job_overhead for EACH of input preparation and output publication.
 * Cross-core release/acquire handoff has an additional declared allowance.
 * A rejected candidate never changes *out. */
int tm_plan_build(const audio_graph_t *graph, const temporal_contract_t *tasks,
                  uint32_t count, const tm_limits_t *limits, uint32_t core_mask,
                  const uint8_t *pins, tm_plan_t *out, tc_admission_t *why);

typedef struct {
  uint64_t (*clock)(void *ctx);
  long (*run)(void *ctx, uint32_t pid, uint64_t budget, uint64_t cutoff);
  int (*prepare)(void *ctx, uint32_t pid, unsigned bank, int previous_valid);
  void (*output)(void *ctx, uint32_t pid, unsigned bank, enum tc_output action);
  void (*kill)(void *ctx, uint32_t pid);
  void *ctx;
} tm_ops_t;
struct tm_runtime;
typedef struct {
  struct tm_runtime *runtime;
  uint32_t index;
} tm_task_ctx_t;
typedef struct {
  struct tm_runtime *runtime;
  uint32_t cpu;
} tm_worker_ctx_t;
typedef struct {
  temporal_scheduler_t scheduler;
  tc_task_state_t published;
  uint32_t stat_seq, owner_pid;
  uint64_t done, generation, epoch_version;
} tm_job_t;
typedef struct tm_runtime {
  tm_plan_t plans[2];
  uint8_t mapping[2][TC_MAX_TASKS];
  uint32_t active_plan, prepared_plan;
  tm_job_t jobs[TC_MAX_TASKS * 2]; /* stable state: no IRQ-time relocation */
  tm_task_ctx_t task_ctx[TC_MAX_TASKS];
  tm_worker_ctx_t worker_ctx[TM_CORES];
  audio_worker_t *worker[TM_CORES];
  tm_ops_t ops;
  uint32_t gate, valid, bank, previous_valid, started;
  uint64_t frame, release, epoch, generation, epoch_version;
  uint64_t accepted, skipped, core_late[TM_CORES];
} tm_runtime_t;
/* Prepare on the serialized control CPU; no mutation of active task state.
 * Commit is a constant-size configuration pointer/index publication at a
 * drained frame boundary. Reuse task state by PID across core migration. */
int tm_prepare(tm_runtime_t *r, const tm_plan_t *plan);
int tm_commit(tm_runtime_t *r, int preserve_epoch);
void tm_cancel_prepare(tm_runtime_t *r);

int tm_init(tm_runtime_t *r, audio_worker_t *workers[TM_CORES],
            const tm_ops_t *ops);
void tm_request_pause(tm_runtime_t *r);
int tm_paused(const tm_runtime_t *r);
int tm_drained(const tm_runtime_t *r);
/* Only while paused. Preserve lifetime/offence state by PID across placement
 * changes; start a new release epoch (intentional downtime isn't a miss). */
int tm_install(tm_runtime_t *r, const tm_plan_t *plan);
/* At a drained frame boundary: retain the release epoch, feedback history,
 * and existing per-PID state. New nodes join the same global phase. */
int tm_replace(tm_runtime_t *r, const tm_plan_t *plan);
int tm_resume(tm_runtime_t *r);
/* Exclusive cadence producer. Atomically admit an entire frame to all cores,
 * or skip it if ANY previous worker is unfinished. Never overwrite in-flight
 * sample storage; never wait on a worker. -1 paused; 0 late; 1 published. */
int tm_kick(tm_runtime_t *r, uint64_t frame, uint64_t nominal_release);
int tm_snapshot(const tm_runtime_t *r, uint32_t pid, tc_task_state_t *out);
#endif
