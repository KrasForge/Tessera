# Temporal-contract verification — 2026-09-16

Repository: KrasForge/Tessera. Base: `720cf6a`. Working branch:
`fix/m12-budget-verification`. Changes remain local and uncommitted. This
record reports local execution, not a GitHub Actions run or physical hardware.
The pre-existing M12 changes were preserved. The pre-extension working files
were backed up in `../Tessera-pre-temporal-1789580174.tar.gz`.

## Later independent verification: intermittent legacy failures

The successful runs recorded below were followed by independent runs which
still found occasional failures **after** the 250 us headroom adjustment.
One M12 run and one of five direct budget repeats missed a worker block;
a later full M12 rerun reached the separate latency fixture and recorded one
watchdog overrun in 3,000 callbacks. The new temporal aggregate and five
additional runs of each temporal fixture passed. These observations mean the
broader legacy gate is **not consistently green**, even though the successful
runs below remain valid evidence. See `temporal-independent-verification.md`,
`build/arm/temporal-validation/final-m12.log`, and `final-m12-rerun.log`.
No missed-block thresholds or CI retry loops were loosened to hide the failures.

## Environment and commands

Development container `tessera-m12-build`, Ubuntu 24.04, GNU AArch64 GCC
13.3.0 and QEMU 8.2.2, `virt` / Cortex-A72 / two cores. Kernel C is compiled
with `-mgeneral-regs-only`. Host tests use GCC with ASan/UBSan; the concurrent
scheduler test was additionally compiled and executed with ThreadSanitizer.

```sh
make -B arm CROSS_COMPILE=aarch64-linux-gnu-
make test-arm-m12 CROSS_COMPILE=aarch64-linux-gnu-
make test-arm-sandbox-qemu test-arm-fault-qemu CROSS_COMPILE=aarch64-linux-gnu-
make test-arm-temporal-all CROSS_COMPILE=aarch64-linux-gnu-
```

All final commands passed. The temporal aggregate includes the kernel build,
three portable test executables and both actual-EL0 QEMU harnesses.

## Portable execution

| Test | Observed result |
| --- | --- |
| Main temporal suite | 235,868 checks passed under ASan/UBSan. |
| Randomized admission/execution property | 2,000 generated graphs: 1,216 admitted and 784 rejected; admitted graphs ran 24 frames each within declared execution/overhead bounds, with no reserved deadline misses. |
| Concurrent plan handoff | 10,000 SPSC control-to-worker updates passed; ThreadSanitizer run also passed without a reported race. |
| Additional scheduler review | 112,641 assertions passed, including another generated-set sweep and old-period accounting across pending rate updates. |
| Kernel-adapter lifecycle review | 825 assertions passed, including 100 bind/run/unbind cycles and a kick after all bindings were removed. |

These checks validate the implemented frame-synchronous model; they are not a
formal schedulability proof or a measurement of hardware interrupt/WCET bounds.

## Actual isolated AArch64 execution

The eight-plugin, 200 Hz all-policy test observed:

| Instance / policy | Runs | Successful | Budget offences | Shed | Killed |
| --- | ---: | ---: | ---: | ---: | --- |
| HARD good plugin | 100 | 100 | 0 | 0 | No |
| Half-rate transient offender / bypass | 50 | 48 | 2 | 0 | No |
| Immediate kill | 4 | 3 | 1 | 0 | Yes |
| Three consecutive offences | 6 | 3 | 3 | 0 | Yes |
| Persistent mute | 100 | 3 | 97 | 0 | No |
| Persistent bypass | 100 | 3 | 97 | 0 | No |
| Degrade by skipping two releases | 36 | 3 | 33 | 64 | No |
| BEST_EFFORT, quarter-rate, insufficient slack | 0 | 0 | 0 | 25 | No |

All 100 callbacks completed with zero worker skips, output-ring underruns,
audio-watchdog overruns, or reserved deadline misses. Every node published a
valid full output block every base frame, including silence/bypass when not due.
The assertions verify partial-output erasure, exact dry-input replacement,
actual process/liveness death, and refusal to re-enter killed processes.

The separate six-plugin acceptance test uses a **750 Hz synthetic cadence**
(48 kHz / 64-sample-equivalent). It passed 128/128 HARD output blocks and
64/64 half-rate jobs, transient bypass and recovery, real memory-fault
containment, three-strike termination and 128 shed best-effort jobs. A live
kernel-control budget update succeeded after an unadmittable update was
rejected without altering the active plan. Zero underruns, watchdog overruns,
worker skips, or reserved deadline misses were observed.

Both harnesses verify a real timer IRQ abort at an absolute cutoff shorter
than the plugin's relative budget. The all-policy harness also verifies the
actual six-register EL0 control syscall (including a period larger than 32
bits), invalid flags/ranges/PIDs, pending-plan rejection, and a malicious
syscall made at a legitimate trampoline SVC address. The latter is rejected:
being inside the trampoline does not authorize arbitrary kernel services.
Allocator counts return to baseline after cleanup.

**Repeatability:** each temporal QEMU harness passed ten additional direct
runs. The legacy 1 kHz budget harness also passed ten additional direct runs
with its zero-miss assertions unchanged.

## Legacy regression finding and correction

The first broad regression attempt found one skipped worker block in the old
1 kHz M12 budget harness (99/100 good-plugin runs). Its configured budgets
allocated the entire frame, leaving no guaranteed allowance for kernel
entry/exit, timer delivery and publication. This failure is retained in
`build/arm/temporal-validation/m12-initial-headroom-failure.log`.

The two hostile-plugin budgets were changed from one third to one quarter of
the frame (333 us to 250 us at 1 kHz). The good plugin retains its one-third
fair share, leaving at least one sixth of the frame for kernel overhead even
if every plugin consumes its allowance. The required 100/100 good blocks and
zero worker skips/underruns/watchdog overruns were **not relaxed**. The full M12
gate and ten further direct budget-harness repetitions then passed.

The legacy 3,000-callback latency harness reported zero underruns and overruns.
The separate pre-existing per-plugin-accounting harness reported four
underruns over 2,500 callbacks and passed its existing tolerance; it is not a
zero-underrun acceptance result. Sandbox and fault regression targets passed.

## Artifacts and limits

Raw logs are retained in `build/arm/temporal-validation/`; the final aggregate
is `temporal-final.log`, with individual repeat logs for all three QEMU suites.
The source implementation and integration contract are documented in
`docs/temporal-contracts.md`. CI was wired to the temporal aggregate and its
YAML parsed successfully; no hosted CI execution is claimed.

The supported scheduler has block-multiple periods, within-frame deadlines,
criticality-first/dependency-ready EDF, and one exclusive EL0 worker. DEGRADE
reduces execution rate; it does not tune an arbitrary plugin's DSP algorithm.
Kernel configuration may be staged live, but the trusted EL0 control client
must not run concurrently with the EL0 worker. Graph mutation, binding changes,
unload and reclamation require stopping/draining and re-admitting the graph.
Initialization/loader/destruction callbacks are not newly time-bounded.

The common kernel implementation is built and exercised through the QEMU
hosts. The board boot remains a bring-up/self-test image. Physical CM4/I2S
continuity, hardware WCET, arbitrary sporadic/cross-frame EDF and simultaneous
multi-core EL0 execution have not been demonstrated. Existing RWX-segment linker
warnings and the old FAT signedness warning remain outside this change.
