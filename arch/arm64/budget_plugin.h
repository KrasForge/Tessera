/* Budgeted EL0 block invocation; host policy, not part of plugin ABI v1. */
#ifndef ARM64_BUDGET_PLUGIN_H
#define ARM64_BUDGET_PLUGIN_H
#include "budget.h"
#include "plugin_loader.h"

typedef struct {
    plugin_t *plugin;
    uint64_t in_l, in_r, out_l, out_r; /* plugin VAs */
    void *left, *right;               /* trusted host aliases, frames * 4 bytes */
    uint32_t frames;
} budget_plugin_call_t;

/* Invoke only on the exclusive EL0 worker, with its timer PPI enabled and
 * no cadence timer on that core.  Output is unpublished scratch until OK;
 * on MUTE/KILL/FAULT both planes are zeroed before returning.  A threshold
 * kill publishes process death immediately; pm_unload reclaims it later.
 * The caller resolves the current control-plane budget before each block. */
int budget_plugin_run(budget_t *b, budget_plugin_call_t *call, uint64_t *elapsed);

/* Policy-free invocation for temporal contracts. Enforces BOTH the relative
 * budget and an absolute counter cutoff. Returns BUDGET_PREEMPTED (-2),
 * BUDGET_DEADLINE (-3), a plugin fault (<0), or the normal result. The caller
 * owns output invalidation and escalation, and must not publish partial data. */
#define BUDGET_DEADLINE (-3L)
long budget_plugin_invoke(budget_plugin_call_t *call, uint64_t cycles,
                          uint64_t cutoff, uint64_t *elapsed);
#endif
