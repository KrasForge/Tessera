# M12 stabilization results — September 16, 2026

## Verified revision and delivery

The full `test-arm-m12-complete` gate passed locally (exit 0) and in hosted
GitHub Actions (success, attempt 1) for commit `82bc0a271934be231597206bdc58e2a57863d699`.
Published branch: `fix/m12-timing-20260916-203359`.
Run: https://github.com/KrasForge/Tessera/actions/runs/35135217518.
Hosted job: `104925772014`; evidence artifact: `10462133677` (110 files).
Artifact SHA256: `0dcbb58fb60c968d4675f03d3e9308ee667ff0d3240f653a84281cc0aa9f6e5d`.

The narrow timing fixes were integrated into the original working tree on
`fix/m12-budget-verification`, preserving its staging index and concurrent
independent edits. No merge to main occurred. CI covers the immutable commit
above, not every newer local change. The original integrated tree additionally
passed five consecutive budget and five latency fixture executions (exit 0).

## Executed acceptance

| Check | Local committed snapshot and hosted CI |
| --- | --- |
| Full combined gate | Passed |
| Strict budget repeats | 20/20; each requires 100/100 good blocks and zero worker/audio misses |
| Strict latency repeats | 20/20; each requires 3,000 callbacks and zero underruns/watchdog overruns |
| Delayed worker negative control | Correctly rejected for a missed worker release |
| Over-budget callback negative control | Correctly rejected by the unchanged watchdog assertion |
| User-mode, M12, sandbox and fault regressions | Passed |
| Temporal host / scheduler review / adapter suites | 235,868 checks / 112,641 assertions / 825 assertions |
| Separate MTTCG temporal QEMU acceptance | 100/100 and 128/128 hard-audio blocks passed |

The runner never retries a failing execution to obtain a green result. It
requires a zero process exit, a single exact final PASS verdict, no FAIL/PANIC
in the transcript, and an unchanged binary hash. Timeout, crash, missing logs
and ambiguous PASS output fail. Both negative controls build and run actual
injected guest delays; compiler errors, timeouts or unrelated failures do not
count as successful negative controls. Clean binaries are rebuilt afterwards.

The accounting fixture is not a zero-miss performance gate: hosted CI reported
nine underruns and twelve skipped releases per worker across 2,500 callbacks,
within its existing tolerance. User/control/sandbox/fault fixtures still use
their original timeout-after-PASS convention; the repaired timing fixtures
instead shut down normally. These distinctions are not hidden by the aggregate.

## Timing model and unresolved hardware claims

The two previously intermittent timing fixtures now use an explicit fixed
instruction-counted QEMU clock (`shift=3`, 8 ns per guest instruction). This is
functional guest-timing acceptance, not cycle-accurate Cortex-A72 timing/WCET.
No plugin budget, watchdog threshold, miss allowance or cadence was relaxed.
Genuine parallel MTTCG temporal fixtures and portable concurrency tests remain
in the full gate. Wall-clock mode remains available with the same strict
assertions as a host-scheduled diagnostic.

In the clock experiment the budget binary passed 1/3 ordinary wallclock runs,
0/3 wallclock runs pinned to one host CPU, and 3/3 instruction-counted runs on
that same CPU. The latency binary passed 3/3 in each mode; its historical
isolated watchdog failure is not claimed to have a conclusively proven sole
cause. Failed wallclock observations are retained and not called successes.
Physical CM4/I2S timing and hardware worst-case execution time remain unverified.

Runtime and harness fixes: local rather than cross-core TLB invalidation for a
local address-space switch (real page-table update shootdowns retained), valid
FP save/restore offsets, correct startup/stop lifecycle checks, synchronized
counter reads, normal timing-fixture shutdown, and strict evidence collection.

## Reproduction and evidence

```sh
docker exec tessera-m12-build make test-arm-m12-complete CROSS_COMPILE=aarch64-linux-gnu-
docker exec tessera-m12-build make test-arm-timing-wallclock CROSS_COMPILE=aarch64-linux-gnu-
```

Implementation contract: `docs/timing-verification.md`.
Local evidence: `build/arm/timing-stability/`.
Snapshot repeated execution logs: `/home/ik/ChatGPT/Tessera/build/timing-work/20260916-202104/src/build/arm/timing-runs/`.
Full committed snapshot patch: `/home/ik/ChatGPT/Tessera-M12-timing-verified.patch`.
That patch is against base `720cf6a` and includes the earlier M12/temporal
snapshot; do not apply it on top of an already-modified M12 working tree.
