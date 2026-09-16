# M12 verification record — 2026-09-16

Base revision: `720cf6a` (`KrasForge/Tessera`, main).
Working branch: `fix/m12-budget-verification`.
Changes are local; this record does not claim that a GitHub Actions run occurred.

## Environment

Ubuntu 24.04 container on the development workstation, GNU AArch64 GCC 13.3.0,
QEMU 8.2.2 (`virt`, Cortex-A72). Kernel compilation uses
`-mgeneral-regs-only`. No physical Raspberry Pi/CM4 was exercised.

## Executed acceptance gate

```sh
make test-arm-m12 CROSS_COMPILE=aarch64-linux-gnu-
```

Exit status: **0**. This includes the complete AArch64 kernel build, six host
test targets (budget, accounting, process lifecycle, worker, graph control,
latency) under ASan/UBSan and five QEMU test targets (budget, resilience,
control syscalls, per-plugin reporting and latency).

| Check | Observed result |
| --- | --- |
| Budgeted two-core execution | 100 good-plugin blocks / 100 callbacks; no worker skips, ring underruns or audio-watchdog overruns |
| Repeatability | 10 additional wall-clock QEMU runs passed the same zero-miss assertions |
| Hog | Three normal blocks, then exactly three budget offences; partial output erased, actual process death, later entry refused |
| Transient offender | Two muted offences followed by normal audio, no kill |
| Resilience | 10/10 cycles, eight good-plugin blocks per cycle; allocator baseline checked after each cycle |
| Fault coverage | Actual null access, forbidden SVC, fresh-instance wild kernel write and infinite-loop budget kill |
| Budget control syscall | Full 64-bit setting, default reset, invalid range and stale PID rejection |
| Registry lifecycle | 100 load/set-budget/unload cycles without stale budget entries or frame leaks |
| Existing audio-latency harness | 3,000 callbacks, zero underruns, zero overruns |
| Accounting concurrency | 100,000-publication two-thread stress passed; bounded snapshot API edge cases passed |
| Adjacent regression targets | `test-arm-sandbox-qemu` and `test-arm-fault-qemu` passed |
| Workflow validation | `actionlint` 1.7.7 passed after repairing an existing unquoted YAML label and shell conditional |
| Patch hygiene | `git diff --check` passed |

An additional GCC ThreadSanitizer build/run of `plugin_time_test` passed with
no race report. GCC warns that it does not model `atomic_thread_fence`; this
is not a proof of the fence protocol on weak-memory hardware. The earlier
Clang attempt could not link because that container lacked Clang's TSan runtime.

## Scope, warnings and remaining boundaries

Budget IRQ logging is removed, output muting is in shared kernel code, and
termination publishes the same process/liveness state used by the fault path.
Reclamation is deferred until the worker is drained. The plugin ABI exports
and version are unchanged. CI now invokes the aggregate acceptance target,
but the branch has not been committed, pushed or run on GitHub Actions.

The main hardware boot path remains a bring-up/self-test image. These gates
exercise the integrated QEMU host path and common kernel components. They do
not certify analog audio continuity, arbitrary simultaneous multi-core EL0
execution, safe execution of init/loader/destroy callbacks, admission control,
or real-hardware worst-case latency. See `plugin-abi.md` and `demo.md`.

Existing GNU linker RWX-segment warnings and a signedness warning in `fat.c`
remain outside M12. Some older QEMU harnesses intentionally wait after PASS
and are stopped by their Makefile timeout; the overall target checks their
PASS markers. The M12 budget/resilience harnesses now power QEMU off themselves.

Raw logs are retained locally under `build/arm/m12-validation/`; generated
binaries and logs are not source changes.

## Temporal extension follow-up

The subsequent temporal-contract implementation, additional test results and
legacy budget-harness headroom correction are recorded separately in
[`temporal-verification.md`](temporal-verification.md). The transcripts above
are the original M12 run, not results for the later extension.
