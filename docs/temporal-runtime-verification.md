# Managed temporal runtime verification — 2026-09-16

Repository: `KrasForge/Tessera`, base `720cf6a9723f85c931f1fc59a7fcd40ef71e1bd6`.
Working branch: `fix/m12-budget-verification`. All edits are local and
uncommitted. No push, issue closure or hosted CI execution is claimed.
Pre-existing work was preserved; the pre-finish working files were backed up
in `../Tessera-pre-finish-1789582183.tar.gz`.

## Completed integration

`temporal_runtime.{h,c}` joins the admitted scheduler to the plugin manager,
worker and I/O adapter. It handles finite lifecycle budgets, transactional
load/admission rollback, guarded topology edits, automatic unbind/reclamation,
and restart after an explicitly paused interval. Public PM/GC mutations cannot
bypass the runtime guard. A bound or executing plugin cannot be unloaded.
The pause protocol gates a racing cadence producer with one atomic ownership
word; it does not rely on a caller merely observing that a worker is idle.

EL0 return state, current-process identity, entry interrupt mask and budget
state are per core. Independent admitted hosts can execute on separate DSP
cores while a trusted control process runs on another core. Nested calls
preserve the caller's FP/SIMD/control state and the earlier outer deadline.
The banked virtual budget timer is independent of the physical cadence timer.
Lifecycle parameter calls use the correct float argument register, and void
DSP returns are normalized. Partial loader failures reclaim owned allocations.

The managed runtime protects ABI negotiation, initialization, direct parameter
callbacks and destruction with finite budgets. Legacy low-level callers which
explicitly retain a zero lifecycle budget are not silently reclassified as
protected. The DSP ABI version and exported function signatures remain unchanged.

## Environment

Ubuntu 24.04 development container `tessera-m12-build`, GNU AArch64 GCC 13.3.0,
QEMU 8.2.2, Cortex-A72 `virt` target. Kernel C is built with
`-mgeneral-regs-only`; assembly preserves FP state around nested EL0 calls.
Portable suites run with ASan/UBSan. Separate ThreadSanitizer executions cover
the scheduler's concurrent configuration handoff and the worker pause race.
No physical board, audio codec or I2S transport was exercised.

## Executed final commands

```sh
make -B arm CROSS_COMPILE=aarch64-linux-gnu-
make test-arm-temporal-all CROSS_COMPILE=aarch64-linux-gnu-
make test-arm-m12 CROSS_COMPILE=aarch64-linux-gnu-
make test-arm-user-qemu test-arm-sched-qemu test-arm-sandbox-qemu \
     test-arm-fault-qemu test-arm-plugin-load-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

The complete command chain exited **0**. The temporal aggregate was run again
after the final runtime ownership/control changes and exited **0**. It includes
the kernel build, the portable suites, all-policy and 750 Hz acceptance
fixtures, worker-pause tests, graph-control tests and the four-core runtime.

The working tree's timing runner selects explicit instruction-count mode for
legacy budget/latency gates by default. Those successes are deterministic
functional timing evidence, **not wall-clock or silicon timing guarantees**.
Legacy fixtures which intentionally idle after their PASS message still end
through the Makefile's documented timeout and exact PASS check; the temporal
and managed-runtime fixtures exit QEMU themselves.

## Observed outcomes

| Gate | Result |
| --- | --- |
| Temporal portable policy/admission suite | 235,868 checks; 10,000 concurrent plan updates. |
| Generated admission/execution property | 2,000 graphs: 1,216 admitted, 784 rejected; admitted graphs execute within the declared synthetic model. |
| Additional scheduler review | 112,641 assertions. |
| Low-level host lifecycle review | 825 assertions, including 100 bind/run/unbind cycles. |
| Worker pause race | 40,006 checks, including 10,000 cycles while the producer and worker continue running. |
| ThreadSanitizer | Both scheduler and worker-pause suites passed without a reported race. |
| Graph-control regression | Mutation guards and failures mapping either endpoint leave the previous graph intact and free the new ring. |
| Eight-plugin policy fixture, 200 Hz | All five policies, actual timer preemption, output erasure/bypass, real termination and 100/100 hard-plugin blocks. |
| Six-plugin acceptance fixture, 750 Hz | 128/128 hard blocks, half-rate work, live admitted update, fault containment and zero checked audio/worker misses. |
| Four-core managed runtime, 200 Hz | Three hard jobs per frame completed all 128 frames across two DSP workers; a concurrent EL0 controller completed 16 real contract SVC updates. |
| Same-core timers | Physical cadence continued for multiple ticks while CPU0 ran a hanging EL0 lifecycle call terminated by the virtual budget timer. |
| Nested lifecycle | A hung ABI call inside the controller's load syscall timed out; the outer control process and its FP register value were restored. |
| Lifecycle attack fixtures | Hangs in ABI/init/parameter/destructor callbacks were contained; every fixture returned the allocator to baseline. |
| Managed graph/lifetime | Actual source-to-gain stereo output checked bit-for-bit; rejected admission rolled back the load; destructive live mutations were refused. |
| Managed restart/reclaim | 100 load/unload cycles, a 32-frame restart without false missed-release accounting, hung-destructor reclamation and complete final allocator/ring recovery. |
| M12 regression | Budget, resilience, control, accounting and latency gates passed; the 3,000-callback latency fixture reported zero underruns/overruns in deterministic mode. |
| Adjacent kernel regressions | EL0/SVC, context switching, sandbox, fault containment and plugin loading passed. |

## Repetitions and retained failures

Each new temporal fixture was repeated ten times directly in normal wall-clock
QEMU mode, with no retry-until-success loop:

| Fixture | Passed |
| --- | ---: |
| Four-core managed runtime | 10 / 10 |
| Eight-plugin temporal policies | 10 / 10 |
| 750 Hz temporal acceptance | 10 / 10 |
| Legacy 1 kHz budget fixture, wall-clock mode | **9 / 10** |
| Legacy budget fixture, deterministic instruction-count mode | 10 / 10 |

The failed wall-clock legacy run is retained as
`build/arm/temporal-finish/budget-repeat-2.log`. It completed 100 cadence
callbacks with no ring underrun/watchdog overrun, but skipped one worker block:
99/100 good-plugin executions. Hostile-plugin preemption, muting, killing and
allocator recovery still passed. The strict zero-worker-skip assertion correctly
failed. This is **not** counted as a passing wall-clock result. The experiment
is consistent with emulator/host scheduling variability, but does not by itself
prove the cause or establish any physical WCET bound. No assertion was loosened
and no further wall-clock retry was used to replace the recorded failure.

An earlier development-stage 200 Hz temporal run also recorded a deadline
failure in `lifecycle-regressions.log`. The final aggregate and all ten final
repetitions passed. That earlier log is retained rather than erased.

The deterministic budget runner was executed for ten consecutive runs with
its existing explicit timing model and strict verdict checks. Unit tests of
the runner also passed. This separates functional enforcement checks from
wall-clock performance observations instead of pretending they are equivalent.

## Evidence and scope

Raw logs: `build/arm/temporal-finish/`. Key files are `final-aggregate.log`,
`kernel-clean.log`, `m12-regressions.log`, `kernel-regressions.log`,
`scheduler-tsan.log`, `pause-tsan.log`, the per-run repetition logs,
`repeat-summary.json`, and `budget-deterministic.log`.

This completes the managed software path for frame-synchronous contracts:
block-multiple periods, within-frame deadlines, criticality-first/dependency-ready
EDF, bounded execution and configurable policies. Separate graphs may run on
separate worker cores; cross-core partitioning/migration of one graph is not
implemented by this adapter. DEGRADE reduces execution rate, not algorithmic DSP
quality. Topology changes intentionally pause the managed graph; gapless hot-swap
requires an application bypass/parallel graph. Allocation and process creation
remain serialized control work, not concurrently safe arbitrary kernel calls.

I/O mapping, routing and publication remain explicit platform callbacks. The
acceptance adapter consumes actual plugin output but is not an I2S driver.
The main board image remains its bring-up/self-test image. Physical CM4 audio,
worst-case interruption/cache interference, arbitrary sporadic/cross-frame EDF
and hardware deadlines remain outside this software completion claim.

Existing RWX-segment linker warnings and the old FAT signedness warning remain.
CI invokes the expanded aggregate, but only local execution is reported here.
