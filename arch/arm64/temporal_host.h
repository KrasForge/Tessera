/* Kernel adapter: admitted temporal plans -> isolated plugin calls -> output. */
#ifndef ARM64_TEMPORAL_HOST_H
#define ARM64_TEMPORAL_HOST_H
#include "temporal.h"
#include "budget_plugin.h"
#include "graph_control.h"
#include "audio_worker.h"

typedef struct {
    budget_plugin_call_t call;
    const void *input_left, *input_right; /* trusted, stable host aliases */
    /* Optional bounded publication hook; output is already valid when called.
     * Must also publish silence on skipped/dead jobs to keep consumers fed. */
    void (*publish)(void *ctx, uint32_t pid, enum tc_output action);
    void *ctx;
} tc_plugin_binding_t;

typedef struct {
    temporal_scheduler_t scheduler;
    graph_control_t *gc;
    tc_limits_t limits;
    tc_plugin_binding_t binding[TC_MAX_TASKS];
    uint32_t bindings;
    temporal_contract_t desired[TC_MAX_TASKS]; /* single control writer */
    uint32_t desired_count;
    tc_admission_t last_admission;            /* single control writer */
    audio_worker_t *worker;
    uint64_t (*clock)(void);
    int (*control_set)(void *ctx, const temporal_contract_t *contract);
    void *control_ctx; /* managed runtime serializes SVC and kernel edits together */
    int last_result;                         /* worker-owned; read drained */
} temporal_host_t;

void tc_host_init(temporal_host_t *h, graph_control_t *gc,
                   const tc_limits_t *limits, uint64_t (*clock)(void));
/* Bind/unbind, graph rewiring and process reclaim require a stopped kicker
 * and a drained worker. Live contract-only updates use tc_host_stage/set.
 * Bindings must cover every plugin before a plan is admitted. */
int tc_host_bind(temporal_host_t *h, const tc_plugin_binding_t *binding);
/* Call BEFORE pm_unload, with the cadence kicker stopped and worker drained.
 * Drops all host aliases for pid and invalidates the old execution plan.
 * Re-admit the remaining graph before resuming kicks. Pending updates make
 * this operation return EBUSY, never free an in-flight binding. */
int tc_host_unbind(temporal_host_t *h, uint32_t pid);
int tc_host_stage(temporal_host_t *h, const temporal_contract_t *c, uint32_t count);
int tc_host_set(temporal_host_t *h, const temporal_contract_t *c);
/* Attach to an EMPTY per-core EL0 worker before it starts. The cadence core
 * must use aw_kick_at, not aw_kick, to carry real absolute deadlines. */
int tc_host_attach(temporal_host_t *h, audio_worker_t *worker);
/* Stop the cadence producer, stop and drain the worker before detaching.
 * Detach releases the exclusive execution slot; mappings may then be freed. */
int tc_host_detach(temporal_host_t *h);
/* Make this host the target of the trusted control-process syscall #13.
 * Binding itself is kernel-only and performed before control clients run. */
void tc_host_bind_control(temporal_host_t *h);
#endif
