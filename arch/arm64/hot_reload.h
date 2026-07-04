/* arch/arm64/hot_reload.h - plugin hot-reload without a dropout
 *                           (Theme A: reliability)
 *
 * Replace a plugin's ELF live - a fast dev loop and a field-update path - with
 * no gap in the audio.  This is safe precisely because plugins are
 * MMU-isolated: the new version is loaded into its own fresh address space while
 * the old version keeps running in its own, so the two coexist with no shared
 * state, and the swap is a single pointer flip at a block boundary.  A
 * single-process host cannot do this safely - swapping code under a running
 * engine risks the whole process.
 *
 * This module is the pure orchestration core: a small state machine that
 * sequences the reload so that
 *
 *   - the running version produces every block until the new one is fully
 *     loaded and initialised (no block is ever produced by a half-ready
 *     version - the no-dropout guarantee);
 *   - the swap commits at exactly one block boundary, after which the old
 *     version is retired (its address space freed);
 *   - a load that fails leaves the running version in place, unchanged (no
 *     dropout, no regression);
 *   - only one reload is in flight at a time, and generations advance
 *     monotonically (0, 1, 2, ...), so a retired version never runs again.
 *
 * The kernel side that actually loads/unloads the isolated processes is the
 * plugin manager (pm_load / pm_unload); this core just decides, per block,
 * which generation runs and when the swap happens.  It is pure and unit-tested
 * on the host (make test-arm-hot-reload) and driven against the real plugin
 * manager end to end on QEMU virt (make test-arm-hot-reload-qemu).
 */

#ifndef ARM64_HOT_RELOAD_H
#define ARM64_HOT_RELOAD_H

#include <stdint.h>

/* Returned as the "retired" generation when no swap happened this block. */
#define HR_NONE 0xFFFFFFFFu

typedef enum {
    HR_STEADY = 0,   /* only the running generation is producing        */
    HR_PREPARING,    /* a new generation is loading; old still running  */
    HR_ARMED,        /* new generation ready; swap commits next block   */
} hr_phase_t;

typedef struct {
    uint32_t active;     /* generation currently producing (starts at 0) */
    uint32_t pending;    /* generation being prepared, or HR_NONE        */
    uint32_t phase;      /* hr_phase_t                                   */
    uint32_t swaps;      /* reloads completed                            */
    uint32_t prepares;   /* reloads begun                                */
    uint32_t aborts;     /* prepares that failed to load                 */
} hr_state_t;

/* Reset: generation 0 running, nothing pending. */
void hr_init(hr_state_t *s);

/* Begin loading a new generation (active + 1).  Returns 1 if accepted, or 0 if
 * a reload is already in flight - only one at a time. */
int hr_prepare(hr_state_t *s);

/* Report the outcome of the load started by hr_prepare: ok != 0 arms the swap;
 * ok == 0 aborts back to steady, leaving the running generation in place (no
 * dropout, no regression) and counting an abort.  Returns the new phase.
 * Ignored (returns the current phase) if no reload is preparing. */
int hr_ready(hr_state_t *s, int ok);

/* Which generation must produce THIS block.  If a swap commits on this call,
 * *retired is set to the now-idle generation (the caller unloads it); otherwise
 * *retired is HR_NONE.  The swap only commits from HR_ARMED - i.e. after
 * hr_ready(ok) - so every block returned is a loaded, initialised generation. */
uint32_t hr_next(hr_state_t *s, uint32_t *retired);

/* Current phase (hr_phase_t). */
int hr_phase(const hr_state_t *s);

#endif /* ARM64_HOT_RELOAD_H */
