# Resilience demo: memory, syscall and CPU-budget containment

M8/M12 acceptance uses real AArch64 EL0 plugins with the MMU and exception
vectors enabled on QEMU `virt`. Two complementary harnesses distinguish
logical output containment from timer-driven callback continuity. Neither is
a physical DAC recording or a hardware latency benchmark.

## Run the complete M12 gate

```sh
make test-arm-m12 CROSS_COMPILE=aarch64-linux-gnu-
```

This builds the kernel, runs the budget/worker/accounting/process/graph/latency
host tests with ASan/UBSan, and runs the budget, resilience, control-syscall,
per-plugin reporting and latency QEMU harnesses. The M12 CI job invokes this
same target. Failure is not hidden behind allowances for missed blocks.

## Four-plugin resilience and resource lifecycle

```sh
make test-arm-resilience-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

`good` renders a 440 Hz sine. `crash` dereferences NULL. `evil` issues a forbidden
SVC; a fresh instance initialized in its test-only kernel-write mode separately
attempts the wild write. The kernel write is not assumed to happen after an
already-fatal SVC. `hog` produces three normal blocks, then writes non-zero
samples to both output planes and spins indefinitely in `process_block`.

The common kernel `budget_plugin_run` helper, rather than a harness-only
counter, preempts and clears the hog's partial output. The test checks the
first two mutes, third-strike process death, shared liveness publication,
refusal of later process entry, and silence on post-kill invocations. All eight
logical blocks retain good-plugin sound. Every load/run/kill/unload cycle must
return the frame allocator to baseline, repeated ten times. Removed PIDs must
not retain budget settings.

The resilience harness inspects the actual plugin output directly. It has no
independent DAC cadence timer; its eight-block checks establish output and
lifecycle correctness, not physical no-dropout audio timing.

### Observed QEMU transcript excerpt

From the verification run on 2026-09-16 (timings and allocator counts vary by
build and machine):

```text
hog budget: 1333us of a 5333us block (fair share of 4)
  [budget] kill pid=4 (hog) after 3 consecutive offences (max=1487us budget=1333us) dead=1 mute-leaks=0
  run 1: good-audio-intact + all-three-neutralised = yes
  run 10: good-audio-intact + all-three-neutralised = yes
leak: baseline=32124 after-10x=32124 no-leak=1
checks: passes=10/10 no-leak=1
RESILIENCE: PASS
```

## Concurrent audio cadence and transient recovery

```sh
make test-arm-budget-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

CPU0 services a synthetic 1 kHz audio callback; CPU1 executes `good`, `blip`
and `hog` under finite budgets. `blip` partially writes and spins on calls three
and four, then recovers: both bad blocks must be silent, the next clean block
must be audible, and its strike streak resets. `hog` must terminate after
exactly three consecutive offences; a raw attempt to run it again must fail.

The assertions require **100/100 good-plugin executions, 100/100 callbacks,
zero worker skips, zero DAC-ring underruns, and zero watchdog overruns**.
Both minimum and maximum measured preemption times are checked: every offending
run must finish between its budget and the block interval, not merely the
fastest run. The test inspects actual plugin output for sound/silence, then
feeds a marker stream into a modeled DAC ring. It is not a 48 kHz I2S loopback
or an analog end-to-end latency measurement.

Budget interrupts do not print. Service-time snapshots are published from the
worker; UART rendering and the kill report occur after the worker drains.
The slot's `plugin_time runs=` includes scheduled no-op visits after death;
the separate `hog: runs=` reports actual isolated invocations.

### Observed QEMU transcript excerpt

```text
audio: serviced=100 underruns=0 overruns=0 worst=5422 cyc
worker: kicks=100 blocks=100 overruns=0
good: runs=100 audible=100 offences=0 muted=0 killed=0
blip: runs=100 audible=98 offences=2 muted=2 killed=0 preempt=[350,447]us
hog: runs=6 offences=3 muted=3 killed=1 preempt=[350,363]us
leak: baseline=32132 after=32132 no-leak=1
checks: online=1 ctl=1 cpu0=1 worker=1 hog=1 blip=1 good=1 preempt=1 no-leak=1
BUDGET: PASS
```

Ten additional wall-clock QEMU repetitions of this strict two-core test passed
on the verification machine, each with zero skips/underruns/overruns. This is
repeatability evidence, not a bound on arbitrary emulator hosts or hardware.

## Control-plane and accounting regressions

`test-arm-control-qemu` exercises syscall 12 from a real EL0 host controller:
a budget above 32 bits is preserved, zero clears it, oversized values and stale
PIDs are rejected, and 100 load/set-budget/unload cycles reclaim all resources.
The host budget tests also exercise late normal return, exact-boundary expiry,
fault handling, forgiveness, zero-tick clamping and counter saturation.

The time-accounting board uses atomic payload fields and ordered sequence
validation, with a bounded snapshot retry. A 100,000-publication two-thread
host stress test checks consistent min/max/mean snapshots.

## Hardware and integration boundary

See [the host policy](plugin-abi.md#host-enforcement-m12-issue-78) for exclusive
EL0-worker ownership, timer ownership, unpublished output buffers, and deferred
resource reclamation. M12 does not implement arbitrary simultaneous EL0 calls,
loader/init/destroy budgets, graph admission control or a complete hardware
audio appliance boot path. Those are not implied by these passing tests.

A real Raspberry Pi 4/CM4 recording and physical I2S/latency measurements remain
M10 work. No `resilience-cm4.mp4` or hardware measurements were fabricated.
