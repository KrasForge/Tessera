# M11/M13 verification record — 2026-09-16

The dedicated functional gate `make -j1 test-arm-m11-m13
CROSS_COMPILE=aarch64-linux-gnu-` passed locally, exit 0, on the published source
snapshot. This is not a zero-failure history or a hardware timing certificate.
The source tree was frozen separately while the live worktree was being edited
concurrently; the user's branch and staging index were not reset or replaced.

## Final local results

- Generic temporal policy/adoption suite: 235,884 checks; 10,000 concurrent
  configuration handoffs; includes singleton strike/death-state regression.
- Multicore planner/runtime suite: 65,989 checks, generated admission cases
  and actual pthread worker/dependency execution.
- Session serialization and malformed-input suite: 10,237 checks.
- Actual session transport/control pthread suite: 1,815 checks and 1,200 live
  plan swaps with exact PCM validation. Separate ThreadSanitizer execution
  passed for the stable-job engine; isolated ELF execution is tested in QEMU,
  not claimed for the host stubs.
- Existing shell, shell-graph and patch parser regressions passed. Decimal
  fractions are now supported, with a positive binary32 test plus malformed
  fraction rejection rather than the obsolete '12.5 must fail' expectation.
- Three-core capacity: the same timer-bound DSP workload took 24,286 us
  serially against a 20,000 us period; one-core admission was rejected, and
  three-core execution completed at least 128 jobs per plugin with zero
  budget/deadline failures or skipped worker frames.
- Cross-core producer/gain output was checked as actual stereo PCM, including
  graph persistence/reload. A spinning plugin was placed on CPU1, CPU2 and CPU3
  in turn and terminated after three offences without corrupting the chain.
- Real PL011 serial runner: 18 live load/wire/unwire/unload/affinity changes,
  invalid input rejection, real FAT file persistence, a new emulator process,
  and identical restored PCM hash. Missing, malformed, incompatible and
  partially loadable sessions retained the running scene and allocator
  baseline. The final functional run had zero missing frames, watchdog
  overruns, or plugin budget/deadline failures in the checked phases.

The application profile is included in the saved console result JSON. The
capacity test is **12 kHz / 240 samples (20 ms)**; the serial functional test is
**48 kHz / 240 samples (5 ms)** with 800 us budgets and 4,000/4,800 us deadlines.
These are explicitly declared emulation profiles. They do not establish the
48 kHz / 64-sample cold-start stress target. The interactive application still
defaults to 48 kHz / 64 samples, and the strict diagnostic remains available as
`make test-arm-session-console-fast`.

## Failures retained, not reclassified

The initial hosted run 35142708832 for commit
6805ca8dff349a2979f14181cf58e17cdf18dd47 failed the capacity timing assertion.
Earlier local profiles also failed: first 750 Hz capacity; then occasional
5 ms cold-entry budget/deadline misses; and 750 Hz serial missing-frame checks.
A bounded trace located one three-core failure on the first scheduler entry
with subsequent 128 completions. No missed-block assertion was relaxed or
retry-until-green added. Instead, functional verification was split from the
faster stress diagnostic with the lower-rate profiles stated above.

Runtime/harness fixes include the singleton same-PID adoption fast path,
control/observer WFE/WFI parking, startup-silence accounting, and separating
the intentionally serial overloaded benchmark from the measured parallel
cadence. All previous failure logs remain in the local verification trees and
the initial hosted failure artifact.

A broader earlier M12 regression attempt failed its existing wall-clock
all-policy temporal fixture ('no admitted deadline miss'). This is not a claim
that every repository test or high-rate MTTCG measurement is consistently
passing. M11/M13's dedicated functional gate is reported separately.

## Location and boundaries

Implementation and usage: `docs/m11-m13-workstation.md`. Local logs are under
`build/arm/m11-m13-validation/` inside the immutable release snapshot recorded
in the live repository's `build/arm/m11-m13-validation/release-source.txt`.
The GitHub workflow saves raw logs/result files on success and failure.

No merge to main or issue closure was performed. The patch includes earlier
uncommitted M12 work as well as M11/M13, and is relative to base 720cf6a, not an
incremental patch to apply blindly on the already-modified user worktree.
Physical CM4/SD/I2S, cache/DMA timing and hardware WCET remain M10. The existing
board boot remains the bring-up/self-test image; `run-arm-workstation` starts
the runnable QEMU application. FAT persistence testing uses memory-backed
storage exported/imported between independent emulator processes, not a
physical SD peripheral or a power-loss test.


## Hosted result for the functional-profile revision

Run **35144333046**, commit **becb65f5f6bb225ac982fd4bfcfcf92ffccd19ed**,
finished with **failure**, not success. M11 passed: all three plugins completed
129/129 jobs with zero budget/deadline misses; serial work measured 24,520 us
against a 20,000 us frame; cross-core PCM, faults on CPU1/2/3, and session
save/reload checks passed.

The real serial test completed all 18 live graph swaps with zero missing output
frames and zero audio watchdog overruns, but its strict plugin-budget assertion
failed: source had two budget offences and gain had one. Their maximum observed
elapsed times were 91,398 and 60,070 counter ticks (about 1,462 and 961 us) against
800 us contracts. Neither was killed, and neither recorded a deadline miss.
The hosted run stopped there, so its two-boot sequence was **not completed**.
Earlier local two-boot success must not be attributed to this hosted run.

A subsequent explicit instruction-counted serial diagnostic also failed the
strict live-edit timing conditions (25 missing frames, and per-plugin deadline
misses). It is retained as a failed diagnostic, not substituted for wall-clock
success. Its runner's duplicate accelerator arguments and undefined shift
metadata were corrected; the diagnostic now reaches the actual assertions
rather than failing with a Python argument error.

**Current conclusion:** M11/M13 implementation and local functional acceptance
are present, and M11 passed hosted acceptance. The complete hosted M11/M13 gate
is not verified green; strict M13 timing under emulator scheduling remains an
open acceptance item. No retry-until-green, disabled assertion, hidden failure
or claim of physical audio continuity is part of this result.
