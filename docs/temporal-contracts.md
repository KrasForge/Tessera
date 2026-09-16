# Temporal contracts for the isolated audio worker

Tessera now has an admitted, frame-synchronous temporal-contract execution
host above the M12 CPU-budget primitive. Its implementation is in
`arch/arm64/temporal.{h,c}` and `arch/arm64/temporal_host.{h,c}`. This document
specifies the supported model; it does not assert a hardware timing guarantee.

## Contract and timing model

A `temporal_contract_t` contains `pid`, `period`, `deadline`, `cpu_budget`,
`criticality`, `overrun_policy`, `kill_after`, and `skip_periods`. Times are
64-bit **ARM generic-counter ticks**, not processor-frequency cycles. Convert
using CNTFRQ, not the CPU clock rate.

The audio host supplies a base `frame_ticks` and explicit `frame_overhead` and
`job_overhead` allowances. The latter cover release/dispatch jitter, entry and
exit, timer-interrupt overshoot, output replacement and publication. Tests can
use zero overhead with a synthetic clock; hardware acceptance requires measured,
conservative nonzero allowances.

Supported contracts satisfy:

```text
0 < cpu_budget
cpu_budget + job_overhead <= deadline <= frame_ticks <= period
period % frame_ticks == 0
period <= INT64_MAX / 4
```

All plugins share the first accepted frame as their phase origin. Each plugin
may independently run every one, two, three, or more base frames. A job must
finish in its release frame; execution is not carried over into later frames.
Its absolute deadline is the **nominal** release timestamp plus its own relative
deadline. Worker lateness does not move that deadline.

This is not an arbitrary-release sporadic-task scheduler, Linux CBS, or a
resumable general-purpose preemptive EDF kernel. Within a frame, a selected
plugin finishes or is aborted by its budget/deadline timer. All normal releases
occur at frame boundaries, so no new periodic job arrives midway through that
call. The hardware timer still interrupts an infinite loop inside EL0.

## Scheduling and admission

The planner selects dependency-ready jobs in this order:

1. HARD before SOFT before BEST_EFFORT.
2. Earliest deadline within a criticality class.
3. PID as a deterministic tie-breaker.

This is criticality-first, precedence-constrained EDF, not global EDF across
all classes. Registration order is irrelevant. Same-frame producers execute
before their consumers. A producer's period must divide its consumer's period,
so the producer is due whenever the consumer is due. A same-frame consumer may
not have higher criticality than its producer. Edges into the DAC require a
base-rate producer; explicit one-frame feedback requires base-rate endpoints.
Incompatible multirate wiring is rejected, not silently interpreted as stale
or resampled input.

`tc_plan_build` checks the complete graph, including disconnected plugins.
Every plugin needs exactly one contract. Missing/unknown/duplicate PIDs, invalid
ranges and policies, dependency inversions, incompatible rates and cycles fail
closed. The output plan is unchanged on rejection.

Admission constructs the worst simultaneous-release frame using the same
ordering the worker will execute. It accumulates HARD and SOFT budgets plus
all declared kernel overhead and checks each completion against its deadline,
then the total against the frame boundary. Every later release set is a subset
of that simultaneous frame; omitted jobs retain their bounded fallback-output
cost but consume no DSP budget. This is a conservative sufficient test, not a
maximally permissive schedulability solver. Low average utilization alone does
not establish that short deadlines can be met.

BEST_EFFORT budgets are not reserved. Their bounded fallback/publication cost
is reserved. At runtime a best-effort job runs only when its complete budget
fits before its absolute cutoff and leaves the output-work tail for the
remaining nodes. It cannot displace a HARD or SOFT job. All SOFT reservations
are checked at admission; use BEST_EFFORT for work which is intentionally
optional under normal admitted load.

`tc_admission_t` reports the error, offending PID, required ticks and available
ticks. A rejected live update leaves the running plan and desired contracts
unchanged.

## Enforcement and output policies

`budget_plugin_invoke` arms the banked generic timer at the earlier of the
relative execution budget and the absolute cutoff. The cutoff leaves the
configured output/interrupt allowance before the deadline. A pending timer
cannot be evaded by returning late: elapsed time is checked too. Output remains
unpublished scratch until the result is classified.

| Policy | On an execution-budget offence |
| --- | --- |
| `TC_MUTE` | Replace both output planes with silence; do not automatically kill. |
| `TC_BYPASS` | Replace both planes with the trusted dry input; reject configuration if no dry input is bound. |
| `TC_KILL` | Silence and terminate the process on its first offence. |
| `TC_MUTE_THEN_KILL` | Silence and terminate after `kill_after` consecutive offences; a successful job resets the streak. |
| `TC_DEGRADE` | Silence the failed job, then skip `skip_periods` future releases before trying again. |

DEGRADE is **execution-rate reduction**, not an automatic change to the DSP
algorithm's quality settings. It is intended for optional analysis/background
work; HARD contracts cannot select it. It does not invent a plugin quality API.

A job dispatched too late to fit is shed and gets fallback output. For reserved
classes this records a deadline miss, not a CPU-budget offence: scheduler
lateness cannot wrongly kill an innocent plugin. Missed worker kicks are counted
arithmetically; old work is never replayed in a catch-up burst. Pending contract
changes do not retroactively alter the release counts during a missed-kick gap.
Duplicate, backward and inconsistent frame/timestamp pairs are rejected.

Every configured node gets a full output block on every base frame, including
non-due, degraded, killed and shed nodes. Only successful execution can publish
its scratch samples. Faults always silence and kill, regardless of overrun
policy. Termination publishes process death; mappings are reclaimed later,
after the worker is drained. Changing a killed plugin's contract does not
resurrect its process or reset its strike state.

## Kernel and control-plane integration

Initialize `temporal_host_t` with the graph, limits and counter reader. Bind each
plugin's isolated call, trusted input/output aliases and optional bounded
publication hook with `tc_host_bind`. Bindings must reference sandbox-gated
plugins, have enough storage for the declared frame count, and keep output
private until publication. Input aliases must remain valid and stable during
bypass; publication callbacks must be bounded kernel code, not plugin code.

Stage a complete admitted graph through `tc_host_stage`, then attach the host
to an empty secondary worker with `tc_host_attach`. The host enforces one
exclusive EL0-worker attachment because Tessera's kernel resume context is
still single-slot. The existing raw callback worker remains available for
legacy harnesses; it is not silently converted into a temporal host.

The cadence core must call:

```c
aw_kick_at(worker, frame_sequence, nominal_release_ticks);
```

A skipped kick does not overwrite the timestamp of the call already executing.
The worker adopts an admitted pending plan only at a frame boundary. The
single-writer/single-reader mailbox uses ownership and release/acquire
publication: it does not copy concurrently mutated non-atomic structs under a
seqlock. The control writer gets `TC_EBUSY` rather than overwriting an unconsumed
plan. Audio never waits for a configuration writer.

`tc_host_set` updates one existing PID's contract by re-admitting the entire
desired graph. The trusted control process can use syscall 13,
`SYS_PLUGIN_SET_CONTRACT`, with registers:

```text
x0 = PID
x1 = period ticks
x2 = relative deadline ticks
x3 = execution budget ticks
x4 = criticality | (overrun_policy << 8); other bits must be zero
x5 = kill_after for MUTE_THEN_KILL, skip_periods for DEGRADE, otherwise zero
x8 = 13
```

Live cross-core configuration updates use the **kernel C control API**. The
trusted EL0 control client must run only while the isolated worker is stopped
and drained: two simultaneous EL0 processes (control or DSP) are not supported
by the global kernel resume context. Rejecting a second temporal-host attachment
does not make unrelated raw `run_user` calls on another core safe.

`tc_host_bind_control` selects the kernel host for that syscall. The frozen
DSP ABI exports/version are unchanged. The legacy `SYS_PLUGIN_SET_BUDGET`
registry is used by legacy M12 hosts, not by the admitted temporal host; change
admitted budgets through `SYS_PLUGIN_SET_CONTRACT`/`tc_host_set` instead.

Untrusted DSP plugins cannot invoke the control syscall, even by jumping to
the legitimate SVC instruction inside their trampoline. A sandboxed plugin's
allowed trampoline syscall is only EXIT. This also prevents using that gadget
to enter an unbounded kernel UART write while EL1 masks interrupts. Budgeted
fault paths avoid synchronous UART diagnostics; the worker records fault counts
for later reporting.

Contract-only updates can be staged live. **Graph edits, binding changes,
load/unload and mapping reclamation require stopping the cadence producer and
draining the worker.** Stop the worker, call `tc_host_unbind(pid)` before
`pm_unload(pid)`, and use `tc_host_detach` before discarding the host/worker.
Unbind drops all host aliases for that PID and invalidates the old execution
plan; subsequent stray kicks fail closed rather than using reclaimed pages.
Rebuild and re-admit changed topology before resuming. Raw graph edits
are not automatically turned into an admitted plan. Initialization, loading,
parameter callbacks and destruction are not made time-safe by this block-call
scheduler.

`tc_state` exposes per-PID releases, runs, completion, budget and deadline
failures, missed releases, shedding, muted/bypassed blocks, degradation, faults
and service times. Read it on the owning worker or after that worker has
drained; it is not a concurrent live snapshot API.

## Acceptance

```sh
make test-arm-temporal-all CROSS_COMPILE=aarch64-linux-gnu-
make test-arm-m12 CROSS_COMPILE=aarch64-linux-gnu-
```

The host gate checks admission, all policies, precedence/rates, periodic release
counts, late dispatch, missed kicks, immutable rejected updates and 10,000
concurrent plan handoffs. The QEMU gate executes actual isolated AArch64 ELF
plugins on CPU1 while CPU0 services a timer-driven output ring. It checks all
five policies, independent periods, EDF ordering, an absolute-deadline abort,
a forged trampoline SVC, the real six-register control syscall and allocator
recovery.

The temporal stress harness deliberately uses a 200 Hz / 5 ms frame with
64-sample blocks. Its timing window tests containment and scheduling; it is not
a measurement of 48 kHz / 64-sample device operation. Physical CM4/I2S acceptance,
worst-case timing bounds under hardware interference, and arbitrary simultaneous
multicore EL0 execution remain outside these tests. The main hardware image is
still the bring-up/self-test image; the new execution host is exercised through
the integrated QEMU harness and compiled into common kernel code.

The aggregate temporal gate also runs an independent two-core fixture at
48 kHz / 64 samples (750 blocks/s) for 128 callbacks. It verifies all 128
hard-audio blocks, 64 half-rate jobs, dry-input bypass, real process death,
a memory fault, best-effort shedding, deadline IRQ preemption, live admitted
budget updates, unbind-before-reclaim, and return to the frame allocator
baseline. It is still emulated ring-buffer output, not physical I2S audio.

Independent host regressions exercise 1,500 generated contract sets, old-period
miss accounting across a staged rate change, and 100 bind/run/unbind cycles
under ASan/UBSan. These run as prerequisites of `test-arm-temporal`, alongside
the policy and concurrent SPSC handoff suite. Verification logs are retained
under `build/arm/temporal-validation/`.
