/* Managed temporal runtime: bounded lifecycle + safe graph/worker ownership. */
#ifndef ARM64_TEMPORAL_RUNTIME_H
#define ARM64_TEMPORAL_RUNTIME_H
#include "temporal_host.h"
#include "plugin_mgr.h"

#define TR_EPLUGIN (-16)
#define TR_EIO (-17)
#define TR_ETIMEOUT (-18)

typedef struct {
    /* Map per-instance I/O into plugin-owned pages; host aliases must refer
     * to those mappings. This is a control-path callback, never an IRQ hook. */
    int (*make_binding)(void *ctx, plugin_t *plugin, tc_plugin_binding_t *out);
    /* Release host metadata only. pm_unload owns mapped plugin frames. */
    void (*release_binding)(void *ctx, tc_plugin_binding_t *binding);
    void *ctx;
} tr_io_ops_t;

typedef struct {
    temporal_host_t host;
    plugin_mgr_t *manager;
    audio_worker_t *worker;
    tr_io_ops_t io;
    uint64_t lifecycle_ticks;
    uint32_t sample_rate, frames;
    uint32_t editing, editor_cpu, running, initialized;
    long last_lifecycle_result;
} temporal_runtime_t;

/* Manager must be empty; worker initialized, empty, not yet started. Its
 * graph may already contain DAC/capture nodes. Installs a mutation guard:
 * raw PM/GC mutation and legacy budget writes are rejected while managed. */
int tr_init(temporal_runtime_t *r, plugin_mgr_t *manager, audio_worker_t *worker,
             const tc_limits_t *limits, uint64_t (*clock)(void),
             uint32_t sample_rate, uint32_t frames, uint64_t lifecycle_ticks,
             const tr_io_ops_t *io);
/* Paused control transaction. A finite timeout prevents hanging control if
 * the worker is broken. The pause handshake gates racing IRQ kicks itself.
 * Existing in-flight audio is completed before this returns success. */
int tr_pause(temporal_runtime_t *r, uint64_t timeout_ticks);
/* Admit the entire current graph, reset the release epoch after intentional
 * downtime, and resume IRQ kicks. Retains lifetime stats and killed latches. */
int tr_start(temporal_runtime_t *r);
/* Operations below require pause. No caller-side manual bind/unbind/drain is
 * needed. Load is transactional: timeout, bad I/O or failed admission removes
 * all newly acquired resources. contract.pid is assigned by the manager. */
long tr_load(temporal_runtime_t *r, const char *path, const temporal_contract_t *contract);
int tr_unload(temporal_runtime_t *r, uint32_t pid);
int tr_connect(temporal_runtime_t *r, uint32_t src, uint32_t dst, int feedback);
int tr_disconnect(temporal_runtime_t *r, uint32_t src, uint32_t dst);
/* Live, transactional contract changes. Uses the same admitted host as SVC 13. */
int tr_set_contract(temporal_runtime_t *r, const temporal_contract_t *contract);
/* Pause, bounded destruction, unbind/free every instance, release guards and
 * execution/control ownership. Even a hung destructor is reclaimed; its
 * negative result remains in last_lifecycle_result. */
int tr_shutdown(temporal_runtime_t *r, uint64_t timeout_ticks);
#endif
