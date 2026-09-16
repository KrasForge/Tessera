/* Frame-synchronous temporal contracts for the exclusive EL0 audio worker.
 * All times are generic-counter ticks, NOT CPU clock cycles. See
 * docs/temporal-contracts.md for the admission model and its limits. */
#ifndef ARM64_TEMPORAL_H
#define ARM64_TEMPORAL_H
#include <stdint.h>
#include "audio_graph.h"

#define TC_MAX_TASKS GRAPH_MAX_NODES
#define TC_OK 0
#define TC_EINVAL (-1)
#define TC_ENODEV (-2)
#define TC_EADMISSION (-3)
#define TC_EDEPENDENCY (-4)
#define TC_EBUSY (-5)
#define TC_ESTALE (-6)
#define TC_RUN_BUDGET (-2L)
#define TC_RUN_DEADLINE (-3L)

enum tc_criticality { TC_HARD, TC_SOFT, TC_BEST_EFFORT };
enum tc_policy { TC_MUTE, TC_BYPASS, TC_KILL, TC_MUTE_THEN_KILL, TC_DEGRADE };
enum tc_output { TC_OUTPUT_OK, TC_OUTPUT_SILENCE, TC_OUTPUT_BYPASS };

typedef struct {
    uint32_t pid;
    uint32_t criticality;
    uint64_t period;
    uint64_t deadline;
    uint64_t cpu_budget;
    uint32_t overrun_policy;
    uint32_t kill_after;          /* required only for MUTE_THEN_KILL */
    uint32_t skip_periods;        /* DEGRADE: skip 1..1024 future releases */
} temporal_contract_t;

typedef struct {
    uint64_t frame_ticks;
    uint64_t frame_overhead;      /* release jitter + frame dispatch WCET */
    uint64_t job_overhead;        /* dispatch, timer overshoot, fallback, publish */
} tc_limits_t;

typedef struct {
    tc_limits_t limits;
    temporal_contract_t task[TC_MAX_TASKS];
    uint32_t deps[TC_MAX_TASKS];
    uint32_t order[TC_MAX_TASKS];
    uint32_t count;
    uint64_t reserved_ticks;      /* worst simultaneous HARD + SOFT release */
} tc_plan_t;

typedef struct {
    int code;
    uint32_t pid;
    uint64_t required;
    uint64_t available;
} tc_admission_t;

/* Pure control-plane operation. Every plugin in graph needs one contract.
 * Transactional: *out is untouched on ANY rejection. Independent jobs may
 * have different rates. A same-frame producer's period must divide its
 * consumer's; it may not have lower criticality. Edges to DAC and feedback
 * require the base rate. Hard/soft reservations include all overheads.
 * BEST_EFFORT receives no reservation and may be shed. */
int tc_plan_build(const audio_graph_t *graph, const temporal_contract_t *contracts,
                  uint32_t count, const tc_limits_t *limits,
                  tc_plan_t *out, tc_admission_t *why);

typedef struct {
    uint32_t pid, killed, streak, skip_left;
    uint64_t releases, runs, completed, budget_overruns, deadline_misses;
    uint64_t missed_releases, shed, muted, bypassed, degraded, faults;
    uint64_t service_ticks, service_max, activated_frame;
} tc_task_state_t;

/* Kernel-only callbacks. run MUST enforce both the relative budget and the
 * absolute cutoff; all other hooks must be bounded within job_overhead.
 * No untrusted plugin pointers/cursors may be used by output/kill hooks.
 * run returns >=0, TC_RUN_BUDGET, TC_RUN_DEADLINE, or another negative fault.
 * output publishes a full valid block, including silence for non-due jobs. */
typedef struct {
    uint64_t (*clock)(void *ctx);
    long (*run)(void *ctx, uint32_t pid, uint64_t budget, uint64_t cutoff);
    void (*output)(void *ctx, uint32_t pid, enum tc_output action);
    void (*kill)(void *ctx, uint32_t pid);
    void *ctx;
} tc_ops_t;

typedef struct {
    tc_plan_t active, pending;
    tc_task_state_t state[TC_MAX_TASKS];
    uint32_t pending_ready;       /* SPSC ownership: release/acquire, no seqlock */
    uint32_t active_valid;
    uint64_t configured_frame_ticks; /* control-writer owned */
    uint64_t first_frame, last_frame, last_release;
    uint32_t started;
} temporal_scheduler_t;

void tc_scheduler_init(temporal_scheduler_t *s);
/* Single control writer stages only admitted plans. A pending plan is never
 * overwritten; EBUSY leaves it intact. Worker adopts at its next boundary.
 * Caller must keep the graph and kernel I/O bindings alive until drained. */
int tc_stage(temporal_scheduler_t *s, const audio_graph_t *graph,
             const temporal_contract_t *contracts, uint32_t count,
             const tc_limits_t *limits, tc_admission_t *why);
/* Exactly once per accepted audio kick, with the NOMINAL release timestamp,
 * not a timestamp taken after worker delay. Duplicate/backward frames are
 * rejected; skipped kicks are accounted without replay/catch-up execution. */
int tc_run_frame(temporal_scheduler_t *s, uint64_t frame, uint64_t release,
                 const tc_ops_t *ops);
/* Inspect state only on the worker, or after the worker has drained. */
const tc_task_state_t *tc_state(const temporal_scheduler_t *s, uint32_t pid);
#endif
