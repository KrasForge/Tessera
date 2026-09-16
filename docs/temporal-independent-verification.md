# Temporal-contract implementation: independent verification

> Historical verification of the earlier temporal layer. Current managed-runtime results and remaining timing limitations are in [temporal-runtime-verification.md](temporal-runtime-verification.md).

Date: 2026-09-16. Local branch: `fix/m12-budget-verification`.
Base Git revision: `720cf6a9723f85c931f1fc59a7fcd40ef71e1bd6` plus the existing
uncommitted M12 work. Nothing was committed, pushed, or run on GitHub Actions.

## Implemented and reviewed paths

The frame-synchronous contract layer covers block-multiple periods, per-plugin
within-frame deadlines, CPU budgets, HARD/SOFT/BEST_EFFORT classes,
precedence-aware EDF within each class, policy selection, whole-graph admission,
and SPSC plan adoption. The common kernel adapter uses actual EL0 calls and CNTP
interrupts, not only fake callbacks. Exact limits are in `temporal-contracts.md`.

Independent review added a regression that first reproduced incorrect missed
release accounting when a new period was adopted after a gap. The fix accounts
missed old frames before plan adoption. Additional changes cover bounded fault
handling without realtime UART, explicit unbind-before-reclaim and lifecycle
regressions. The sandbox gate now requires EXIT as well as a valid trampoline
address; a real EL0 test exercises the forged-control-syscall case. The minimal
EL0 fixture's linkage, which broke when process termination was made explicit,
was repaired and re-executed.

## Final temporal gate

Executed, exit status **0**:

```sh
make -j1 test-arm-user-qemu test-arm-temporal-all CROSS_COMPILE=aarch64-linux-gnu-
```

`test-arm-temporal-all` includes the complete AArch64 image build, all three
host test suites and both real-EL0 QEMU fixtures. Raw result lines:

```text
TEMPORAL INDEPENDENT REVIEW: PASS (112641 assertions)
TEMPORAL ADAPTER REVIEW: PASS (825 assertions; 100 bind/run/unbind cycles)
TEMPORAL HOST: PASS (235868 checks; 10000 concurrent plan updates)
syscall-gate: forged contract SVC at a valid trampoline address rejected
audio: callbacks=100 worker-skips=0 underruns=0 watchdog=0
TEMPORAL: PASS
audio: blocks=128 underruns=0 watchdog=0 worker_skips=0
TEMPORAL ACCEPTANCE: PASS
```

The independent acceptance runs 128 callbacks at 750 Hz (48 kHz / 64 samples)
on two emulated cores. It observed 128 good-plugin completions, 64 half-rate
completions, two transient overruns followed by recovery, three consecutive
hog offences followed by actual termination, memory-fault containment,
best-effort shedding, and absolute-deadline timer preemption. Every output
publication was checked, all six bindings were removed before reclaim, and
the physical-page allocator returned to its baseline. Output goes to emulated
rings/markers, not a physical I2S peripheral.

Both QEMU temporal binaries were then run **five additional times each** with
all PASS markers and zero process exit status. Logs:
`repeat-acceptance-1.log` through `-5.log` and `repeat-policies-1.log` through
`-5.log` in `build/arm/temporal-validation/`.

## Legacy regression caveat — do not erase failures

An earlier broad gate completed with exit status 0 (`full-gate.log`), covering
M12, both temporal fixtures, sandbox/fault tests and graph-scheduler regressions.
A subsequent final M12 attempt failed its strict 1 kHz budget fixture with one
worker skip: 99/100 good-plugin runs, while budget termination/muting checks
passed and no audio-ring underrun was observed (`final-m12.log`, exit 2).
Five direct repeats of the same legacy binary then produced **4 PASS / 1 FAIL**;
the failure again had one worker skip. The cause has not been conclusively
isolated. No missed-block threshold was relaxed, and these results are not
reclassified as success. See `legacy-repeat-*.log` and
`legacy-budget-one-missed-block.log`.

The final legacy rerun also exited **2**, this time at the separate latency
harness: 3,000 callbacks and zero underruns, but **one watchdog overrun**
(`final-m12-rerun.log`, `final-m12-rerun.exit`). That binary does not link the
temporal scheduler/host. This is additional evidence of a timing-sensitive
legacy verification boundary, not a justification for ignoring a failed gate.
The final overall M12 regression status is therefore **not consistently green**.
The new temporal aggregate and its ten additional QEMU runs did pass. No retry
loop or increased allowance was added to CI to hide either legacy failure.

## Reproducibility and scope

Source fingerprints for the final runs are in `final-source.json`. Existing
linker RWX-segment warnings and the signedness warning in `fat.c` remain.
The workflow YAML parsed successfully; no new GitHub Actions execution or
hardware result is claimed. `git diff --check` passed.

DEGRADE means skipping future releases, not adaptive DSP algorithm quality.
Periods are audio-frame multiples; deadlines stay within one frame. No arbitrary
sporadic releases, resumable mid-job EDF, shared multicore EL0, automatic graph
mutation/admission handoff, or physical CM4/I2S timing proof is claimed. Plugin
initialization and loader callbacks remain outside this block-call contract.

The original dirty tree was backed up as
`/home/ik/ChatGPT/Tessera-before-temporal-20260916-193620.tar.gz`.
The incremental temporal patch is `/home/ik/ChatGPT/Tessera-temporal-contracts.patch`;
it applies on top of that pre-task working tree, not on a clean `main` without
its M12 changes. Its application was checked against a reconstructed baseline.
