/* arch/arm64/io_quota.h - per-plugin syscall / I/O-rate quota (Theme M22,
 * issue #198)
 *
 * The resource-isolation story had two of its three legs already: the M12 CPU
 * budget (budget.c) bounds a plugin's compute, and the memory quota
 * (mem_quota.h) bounds its RAM.  This closes the last hole - a plugin's
 * SYSCALL RATE.  A plugin that cannot blow CPU or memory can still hammer the
 * SVC gate (or a storage/DMA path) and steal time from the system; this
 * bounds that too.
 *
 * The primitive counts "units" charged in a scheduling window against a
 * ceiling.  It is deliberately generic so one type serves both ceilings the
 * issue asks for:
 *   - syscall rate: charge 1 unit per SVC at the gate (arch/arm64/syscalls.c);
 *   - I/O bandwidth: charge a byte count per storage/DMA-bound operation.
 * Instantiate one io_quota_t per plugin per ceiling.
 *
 * Enforcement has the same two-level shape as the CPU budget:
 *   - IMMEDIATE THROTTLE (per charge): once a window's ceiling is reached,
 *     ioq_charge() refuses further charges - the gate declines the syscall
 *     (returns an error to EL0) instead of servicing it, so a runaway plugin
 *     is bounded WITHIN the window, not merely punished after it.
 *   - ESCALATION (per window): ioq_window() applies the exact truth table of
 *     budget_account() - a window in which any charge was refused is an
 *     offence: the first in a streak THROTTLES, kill_after consecutive
 *     offences KILL (latched); a clean window forgives.  A sustained abuser is
 *     removed through the host's unload path, exactly like an over-budget or
 *     faulting plugin, and the safe-bypass keeps the audio graph running.
 *
 * The accounting is pure and integer-only (the kernel builds
 * -mgeneral-regs-only) and host-tested: make test-arm-iobudget.
 */

#ifndef ARM64_IO_QUOTA_H
#define ARM64_IO_QUOTA_H

#include <stdint.h>

/* ioq_window() verdicts - mirror the CPU budget's BUDGET_OK/MUTE/KILL so the
 * host can treat an over-quota plugin exactly like an over-budget one. */
#define IOQ_OK       0
#define IOQ_THROTTLE 1
#define IOQ_KILL     2

typedef struct {
    uint32_t ceiling;      /* max units chargeable per window (0 = unlimited) */
    uint32_t kill_after;   /* consecutive offence windows that trigger a kill */
    uint32_t used;         /* units charged in the current window             */
    uint32_t peak;         /* high-water units in any single window (stats)   */
    uint32_t streak;       /* current consecutive-offence run                 */
    uint32_t hit;          /* a charge was refused this window (offence flag) */
    uint64_t throttled;    /* total units refused over the plugin's life      */
    uint64_t offences;     /* total offence windows                           */
    uint32_t killed;       /* latched once the kill threshold is reached      */
} io_quota_t;

/* Initialise with a per-window ceiling (0 = unlimited) and a kill threshold
 * (consecutive offence windows; 0 clamps to 1). */
void ioq_init(io_quota_t *q, uint32_t ceiling, uint32_t kill_after);

/* Charge `units` at the gate: 1 per syscall, or a byte count for the I/O-band
 * ceiling.  Returns 1 and charges if it fits within this window's ceiling (or
 * the quota is unlimited and the plugin alive); returns 0 and refuses -
 * charging nothing (all-or-nothing), bumping `throttled`, and arming the
 * window's offence flag - when it would exceed the ceiling, or the plugin is
 * already killed. */
int ioq_charge(io_quota_t *q, uint32_t units);

/* Close the current window and apply the escalation, then reset the per-window
 * counters for the next one.  A window in which any charge was refused is an
 * offence: the first in a streak returns IOQ_THROTTLE, the kill_after-th
 * consecutive returns IOQ_KILL (latched - every later call repeats KILL); a
 * clean window returns IOQ_OK and forgives (streak resets).  Same truth table
 * as budget_account(). */
int ioq_window(io_quota_t *q);

/* Render one plugin's quota state as a libc-free shell line, e.g.
 *   ioq: pid=3 used=12/64 peak=64 throttled=120 offences=2 killed=0
 * for the `prof`/shell view alongside CPU load.  `pid` is the plugin id.
 * Returns characters written (NUL-terminated); `cap` bounds the buffer. */
int ioq_render(const io_quota_t *q, uint32_t pid, char *buf, int cap);

#endif /* ARM64_IO_QUOTA_H */
