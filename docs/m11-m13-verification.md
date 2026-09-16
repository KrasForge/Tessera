# M11 / M13 verification — 2026-09-16

## Tested implementation

The final source snapshot includes the existing uncommitted M12 work on base
`720cf6a9723f85c931f1fc59a7fcd40ef71e1bd6` plus the M11/M13 integration. Work was
isolated from concurrent edits in the original dirty working tree. The
original branch and index were not reset or replaced. A dedicated verified
branch/worktree is used for delivery; hosted CI results are tied to its commit,
not to unrelated later changes in the original working directory.

## Local execution

The final new-code command passed with exit status zero:

```sh
make test-arm-m11-m13 test-arm-session-tsan CROSS_COMPILE=aarch64-linux-gnu-
```

The combined M12 regression gate also passed locally before the final narrow
store-before-SEV ordering correction. The dedicated hosted workflow reruns
that complete M12 gate against the final published source, including that
correction. A workflow definition alone is not evidence of a hosted pass.

| Coverage | Observed result |
| --- | --- |
| Multicore planner/executor host suite | 65,989 checks, including 5,000 generated graphs: 1,337 admitted and 3,663 rejected. |
| Codec / malformed input / shell limits | 10,237 checks, including strict 64-bit values, fractional float parameters and malformed/truncated sessions. |
| Actual common engine under threads | 1,815 checks; 1,200 live load/wire/unwire/unload/migration swaps; exact PCM checked on every complete output frame. ASan/UBSan and ThreadSanitizer both passed. |
| M11 actual ELF capacity | Three identical approximately 2 ms plugin jobs take about 6.01 ms in serial against a 5 ms frame. Serial watchdog violation is asserted. Admitted three-core execution completes at least 128 jobs per plugin with no plugin budget offences, deadline misses or skipped frame. |
| Cross-core data path | Source-to-gain stereo output checked sample-by-sample and compared with a one-core reference hash. |
| Fault isolation | A spinning plugin is killed after three offences on each of CPUs 1, 2 and 3; a real memory-write fault is also contained on each core while the reference graph continues. |
| Real serial control | PL011 commands are exchanged with the running kernel, with console on CPU3 and DSP on CPU1/2. Eighteen live configuration swaps preserve the reference signal and do not add missed frames or watchdog overruns in the declared test model. |
| Cold-boot persistence | Save actual FAT files, export the entire volume, exit QEMU, start a second emulator process and reload. Graph, placement, contracts, parameters and the PCM hash match. |
| Failed restore | Missing, malformed, incompatible-clock and partially loadable sessions leave the existing graph/PIDs and signal intact. |
| Real audio plugins | The same shell loads the oscillator and low-pass filter ELF files and runs their connected output path; no mocked shell backend. |
| Interactive persistent frontend | A scripted `:reboot` saves the host image, restarts QEMU and reloads the saved graph. Normal exit and allocator baseline are verified. |
| Existing interfaces | Shell, graph-shell, old patch-format and the broader M12/temporal regression suites passed. |

The host tests use fake callbacks only where testing scheduling math or
threaded ownership. The QEMU tests load AArch64 ELF plugins, execute at EL0,
use actual timer interrupts and check real output buffers. All final emulator
fixtures terminate normally; a missing verdict, fault, timeout or nonzero
exit fails acceptance. No retry-until-success wrapper is used.

## Timing model and retained failures

The new QEMU functional gates declare:

```text
-accel tcg,thread=single -icount shift=0,align=off,sleep=off
```

That is one nanosecond of virtual time per emulated instruction, **not** a
Cortex-A72 hardware calibration. The capacity fixture uses 48 kHz / 240 samples
(5 ms frames); the interactive/cold-boot fixture uses 48 kHz / 64 samples.
M12's earlier strict budget/latency fixtures retain their separate declared
instruction-count model. ThreadSanitizer tests cover genuinely concurrent
host threads; they are not replaced by single-threaded emulation.

Development runs with host-scheduled MTTCG and a slower instruction-count
model produced startup and live-change deadline misses. Initial state-copy
work on the cadence core was removed, per-PID state made stable, worker plan
adoption restricted to live entries, and publication stores completed before
SEV. Control-side polling yields. These corrections do not establish that
all host-wall-clock failures have one cause or that arbitrary host CPU
contention is now harmless. Failed transcripts remain in `build/evidence/`
and earlier `build/m11-m13/` logs. They are not counted as successful repeats.

Physical CM4/SD/I2S, hardware WCET, gapless analog output and power-loss-safe
storage are **not** claimed. The storage test uses a 4 MiB RAM-backed FAT16
volume exported/imported through QMP. The interactive frontend flushes this
volume on normal `:reboot`/`:quit`; abrupt emulator termination can lose writes.
User graph edits can intentionally change the signal, and abrupt gain or
routing changes are not automatically crossfaded.

## Evidence and reproduction

```sh
make test-arm-m11 CROSS_COMPILE=aarch64-linux-gnu-
make test-arm-m13 CROSS_COMPILE=aarch64-linux-gnu-
make test-arm-m11-m13 CROSS_COMPILE=aarch64-linux-gnu-
make test-arm-session-tsan
make test-arm-m12-complete CROSS_COMPILE=aarch64-linux-gnu-
```

Local final transcripts are in `build/evidence/final-publication.log` and
`final-regressions.log`. Serial command transcripts, commands, disk images,
source/binary hashes and cold-boot JSON results are in
`build/arm/m11-m13-validation/`. Additional repetition outcomes are kept in
`build/evidence/repeated/`. The hosted `M11 and M13 integrated acceptance`
workflow uploads evidence on both success and failure.

The actual kernel image still carries pre-existing build warnings such as
FAT signedness and legacy RWX ELF segments. No warning-free build is claimed.
