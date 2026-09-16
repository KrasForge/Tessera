# M12 timing verification

## What the two timing modes establish

`make test-arm-m12-complete CROSS_COMPILE=aarch64-linux-gnu-` is the combined
stabilization gate. It runs M12, the temporal extension, user-mode and fault /
sandbox regressions, runner self-tests, two real injected-delay negative
controls and twenty repetitions of each of the two former flaky fixtures.
A failed repetition fails the target immediately; there is no retry-to-pass.

The strict budget and latency fixtures now explicitly use instruction-counted
QEMU: `-accel tcg,thread=single -icount shift=3,align=off,sleep=off`. This assigns
8 ns of guest virtual time to each executed instruction. The exact zero-miss
assertions and plugin budgets are unchanged. This tests the guest timing /
containment mechanisms under a declared repeatable execution model, **not**
Cortex-A72 worst-case execution time or real-world latency.

QEMU documents that icount is not cycle-accurate, and that it cannot be combined
with multi-threaded TCG. Its fixed-rate virtual clock derives from executed
instructions rather than host scheduling time:

- https://www.qemu.org/docs/master/devel/tcg-icount.html
- https://www.qemu.org/docs/master/devel/multi-thread-tcg.html

The MTTCG measurement remains available with identical strict assertions:

```sh
make test-arm-timing-wallclock CROSS_COMPILE=aarch64-linux-gnu-
# Or change either original fixture individually:
make test-arm-budget-qemu QEMU_TIMING_MODE=wallclock CROSS_COMPILE=aarch64-linux-gnu-
```

Wall-clock emulation runs guest CPUs as host threads. A pause by the host, JIT
translation, or QEMU-wide synchronization can consume an apparent guest budget
without equivalent guest execution. It is useful diagnostic evidence and is
not relabelled as a passing hardware timing test when it fails. Restricting a
QEMU instance to one host CPU amplified the budget fixture's failures while
fixed-instruction-time runs on the same host CPU passed. The old one-off
latency-watchdog failure has not been reproduced in every wall-clock run; no
claim is made that its sole historical cause was conclusively isolated.

The portable temporal concurrency tests and separate MTTCG temporal fixtures
remain in the combined gate. Instruction-counted timing tests do not replace
those concurrent executions, and none of these tests certify physical I2S.

## Fail-closed execution

`scripts/run_qemu_timing.py` records the emulator version, command, binary hash,
process exit and raw serial/stderr logs for every execution in
`build/arm/timing-runs/`. Success requires a zero process exit and exactly one
final fixture PASS line, with no FAIL or panic in the transcript. Timeouts,
crashes, stale/missing logs, non-final PASS and binary changes fail. The legacy
latency fixture now shuts QEMU down after its verdict instead of depending on
an ignored external timeout.

`test-arm-timing-negative` builds a deliberately delayed worker and an
intentionally over-budget audio callback. The normal strict runner must reject
them specifically for the missed worker deadline and watchdog overrun. A
compiler error, timeout, unrelated failure or PASS is not accepted as a
successful negative control. Uninjected binaries are rebuilt afterwards.

## Narrow runtime / harness corrections

- The local EL0 address-space switch no longer broadcasts a full TLB flush to
  all cores. It retains a local invalidation and the barriers; real page-table
  edits in `vmem.c` still perform their broadcast invalidations.
- FP control/status save and restore use individually encodable loads/stores
  at byte offsets 512 and 520 instead of an out-of-range X-register pair offset.
- Budget-fixture startup verification records that the worker was online before
  requesting stop, instead of racing its final offline-state publication.
- The per-plugin accounting fixture separately verifies worker startup, drain
  and final offline publication; it also exits QEMU normally after its verdict.
- Counter reads in the latency fixture are instruction-synchronized. No budget,
  watchdog threshold, miss allowance, sample count or cadence was relaxed.

## Reproduction

```sh
make test-arm-m12-complete CROSS_COMPILE=aarch64-linux-gnu-
make test-arm-timing-stability CROSS_COMPILE=aarch64-linux-gnu-
python3 scripts/run_qemu_timing.py --fixture budget --repeat 20
python3 scripts/run_qemu_timing.py --fixture latency --repeat 20
```

The dedicated `M12 timing and temporal acceptance` workflow runs the same
complete target and uploads logs even when it fails. A workflow definition is
not itself evidence of a hosted run; actual run outcomes are recorded separately.
