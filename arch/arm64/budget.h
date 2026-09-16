/* arch/arm64/budget.h - per-plugin CPU budget enforcement (Issue #78, M12)
 *
 * The sandbox contains memory (MMU, issues #14/#35) and syscalls (SVC gate,
 * issue #35); issue #77 added attribution of time.  This module closes the
 * third leg: a plugin that overspends its per-block CPU budget - by bug or by
 * malice - cannot hold a core.  The kernel preempts it AT ITS BUDGET
 * BOUNDARY, mid-process_block, and the host escalates: mute first, kill
 * after repeated abuse.  This is host policy, not an ABI change - the frozen
 * plugin ABI v1.0 is untouched (docs/plugin-abi.md, "Host enforcement").
 *
 * Preemption mechanism (AArch64 side):
 *   - Immediately before entering a budgeted plugin, the worker core arms
 *     its own banked generic timer (CNTP_CVAL = now + budget) and opens the
 *     EL0 IRQ window (g_user_spsr: the plugin runs with IRQs unmasked; the
 *     kernel itself never does).
 *   - If the plugin returns in time, budget_disarm() closes everything; the
 *     level-triggered PPI deasserts with CNTP_CTL = 0.
 *   - If the timer fires first, the IRQ lands in this core's own vectors
 *     while the plugin is at EL0; budget_timer_irq() (routed by irq.c for
 *     TIMER_IRQ before the scheduler tick) disarms, EOIs, and unwinds with
 *     kernel_resume(BUDGET_PREEMPTED) - exactly the
 *     fault-kill path, but with a distinct code, so the host can tell "over
 *     budget" from "crashed" and apply policy instead of burying the plugin.
 *
 * The armed state is a single slot tagged with the owning core, mirroring
 * the kernel's single-slot EL0 resume context (process.c): exactly one core
 * runs (and therefore budgets) EL0 plugins at a time.  CPU0's cadence timer
 * uses the same INTID on its own banked timer; the routing in irq.c keeps
 * the two apart by core.
 *
 * Escalation policy (pure C, host-tested, make test-arm-budget):
 *   - Every block, budget_account() is told whether the plugin overran.
 *   - An overrun is an offence: the first in a streak MUTES the plugin (the
 *     host emits silence downstream and keeps the graph fed); kill_after
 *     CONSECUTIVE offences KILL it (process death now, reclamation after
 *     workers drain). A
 *     clean block resets the streak - a plugin that recovers is forgiven,
 *     one that abuses the budget repeatedly is removed.
 *   - Budgets default to a fair share of the block period across the
 *     worker's nodes (budget_fair_share) and are settable per plugin through
 *     the control plane (gc_set_budget, issue #30's syscall surface).
 */

#ifndef ARM64_BUDGET_H
#define ARM64_BUDGET_H

#include <stdint.h>

/* run_user()'s return value when the plugin was preempted at its budget
 * boundary (the fault path returns -1; clean SVC exits return >= 0). */
/* Independent EL1 virtual-timer PPI; CNTP remains the cadence timer. */
#define BUDGET_TIMER_IRQ 27u

#define BUDGET_PREEMPTED (-2L)

/* budget_account() verdicts. */
#define BUDGET_FAULT (-1)
#define BUDGET_OK   0
#define BUDGET_MUTE 1
#define BUDGET_KILL 2

/* ---- policy (pure) ---------------------------------------------------- */

typedef struct {
    uint64_t cycles;       /* per-block CPU budget (counter cycles)         */
    uint32_t kill_after;   /* consecutive offences that trigger the kill    */
    uint32_t streak;       /* current consecutive-offence run               */
    uint64_t offences;     /* total offences (stats line)                   */
    uint32_t killed;       /* latched once the kill threshold is reached    */
} budget_t;

void budget_init(budget_t *b, uint64_t cycles, uint32_t kill_after);

/* The default budget: a fair share of the block period across n nodes. */
uint64_t budget_fair_share(uint64_t block_cycles, uint32_t n_nodes);

/* Account one block: `overran` != 0 when the plugin hit its budget boundary.
 * Returns BUDGET_OK, BUDGET_MUTE (offence: emit silence downstream), or
 * BUDGET_KILL (threshold reached; latched - every later call repeats KILL). */
int budget_account(budget_t *b, int overran);

/* Shared enforcement, not just a policy counter.  All callbacks are required
 * and must be bounded kernel operations except run(), which enters EL0.
 * mute() discards ALL partial output; kill() marks the process dead without
 * freeing mappings still in use by the graph.  Reclamation is control-plane
 * work after the worker drains.  No UART, allocation, or graph edits here. */
typedef struct {
    uint64_t (*clock)(void);
    long (*run)(void *ctx);
    void (*mute)(void *ctx);
    void (*kill)(void *ctx);
} budget_ops_t;

/* A late normal return also counts: it may beat delivery of a pending IRQ.
 * elapsed covers the isolated call, not output cleanup or reporting.
 * A killed policy never invokes run/kill again, but still supplies silence. */
int budget_execute(budget_t *b, const budget_ops_t *ops, void *ctx,
                   uint64_t *elapsed);

/* ---- preemption (worker core; no-ops in host builds) ------------------ */

/* Arm this core's budget timer for `cycles` from now and open the EL0 IRQ
 * window.  Call immediately before entering the plugin. */
void budget_arm(uint64_t cycles);
/* Bounded non-audio EL0 call (ABI/init/parameter/destructor). Nested calls
 * inherit the earlier outer deadline; restore its timer/SPSR on return.
 * Negative BUDGET_PREEMPTED denotes timeout. Missing support fails closed. */
long budget_call(long (*run)(void *), void *ctx, uint64_t cycles);


/* Close the window after the plugin returned in time. */
void budget_disarm(void);

/* TIMER_IRQ router hook (strong version of the weak default in irq.c):
 * returns 1 when the IRQ was this core's budget expiry - in which case, if
 * the core was at EL0, it does not return (kernel_resume) - and 0 when the
 * IRQ belongs to the cadence timer (CPU0). */
struct trapframe;
int budget_timer_irq(struct trapframe *tf, uint32_t iar);

#endif /* ARM64_BUDGET_H */
