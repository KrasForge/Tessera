# M11 / M13: multicore DSP and the serial workstation

The shared implementation is `temporal_multicore`, `audio_session`,
`session_shell`, and `session_preset`. The interactive QEMU application and
acceptance tests invoke these same kernel components; they do not implement a
second scheduler in the shell or merely print expected result strings.

## Run the workstation

```sh
make run-arm-workstation CROSS_COMPILE=aarch64-linux-gnu-
```

The QEMU application uses CPU0 for the 48 kHz / 64-sample-equivalent cadence,
CPU1/2 for isolated DSP, and CPU3 for serial control. Reserving a control core
keeps file I/O, ELF parsing, initialization and UART printing off the audio IRQ.
The scheduler also supports CPU1/2/3 together; the separate capacity fixture
exercises all three with control performed outside the measured audio window.

A patch can be built without editing or compiling a new C test:

```text
load /sd/SYNTH.ELF
load /sd/GAIN.ELF
wire 1 2
wire 2 dac
contract 1 hard 400us deadline=1000us policy=mute
contract 2 soft 400us deadline=1200us policy=bypass
pin 1 2
pin 2 1
set-param 2 0 0.5
start
stats
ls
patch save /sd/LIVE.TSP
```

The example PIDs assume a fresh boot; `load` prints the actual PID. `help`
lists commands, `pause` stops DSP after draining, and `quit` exits the QEMU
application. `wait <callbacks>` and `inspect` are observation commands for the
QEMU backend; inspect reports actual interleaved PCM16 samples and a block
hash, not the audio-hardware output. The current QEMU backend does **not** play
the signal through physical I2S or the host speakers.

`load`, `unload`, `wire`/`connect`, `unwire`, `set-param`, `contract`, `cores`,
`pin`, `ls`/`contracts`, `stats`, `patch save|load|ls`, `start`/`run`, `pause`,
and `clear` are implemented. Oversized lines and extra arguments fail as a
whole: a truncated destructive prefix is never executed. PIDs, times and
parameter bit patterns reject integer overflow. Parameter values accept
signed decimal integers/fractions (up to nine fractional digits) or exact
`0x` binary32 bit patterns. Scientific notation is not supported.

## Temporal contracts and core assignment

Periods are integral multiples of the audio frame; deadlines remain inside
the release frame. All times use generic-counter ticks internally. The shell
accepts positive integer `us`, `ms`, `ticks`, or `blocks` durations. For example:

```text
contract 2 soft budget=300us deadline=1200us period=2blocks policy=degrade skip=2
```

This particular slower-rate contract is rejected when its output feeds a
base-rate DAC without an explicit rate conversion. Admission never silently
reuses stale samples to make an incompatible graph appear valid.

Admission considers task criticality, dependency order, core availability,
per-task budget, input/output overhead, and cross-core handoff allowance. It
uses deterministic earliest-finish core placement in the precedence-aware,
criticality/EDF order and backward deadline constraints for successors. `pin`
constrains placement; `auto` removes that constraint. `cores` changes the
available prefix of DSP CPUs and re-admits the complete graph. The interactive
CPU3 control application therefore rejects `cores 3`; the three-worker
capacity application permits it.

This is a conservative sufficient admission test, not an optimal bin-packing
solver, global migratory EDF, or support for arbitrary sporadic releases. A
rejection includes the offending PID and required/available ticks when the
admission result supplies them. No rejection replaces the active graph.

Same-frame cross-core edges wait only until the successor's safe latest
start. A missing producer yields bounded silence rather than an unbounded
barrier or stale-buffer read. Full output is published with release/acquire
ordering. Explicit feedback reads the previous frame's bank. Independent
EL0 processes execute on their assigned cores with the M12 timer, memory and
syscall isolation. Process strike/death state follows the PID across core
migration and contract changes; reconfiguration does not resurrect a killed
plugin.

The transport uses trusted kernel-owned double buffers. Plugins can write
only scratch output, which is copied/published after classifying the result.
Single-source stereo transfers preserve float bit patterns; fan-in saturates
through Q1.31, and the sink converts to PCM16. This is not a zero-copy claim.

## Live graph replacement and reclamation

Control prepares an inactive graph/placement plan while the active graph
continues running. Stable task and plugin instances are retained by PID.
The cadence core collects the completed old frame, publishes the prepared
configuration at a drained boundary, and kicks the next frame. It does not
perform ELF loading, allocation, destruction, UART output, or relocation of
all task state in the audio IRQ. Removed instances are reclaimed by the
control core only after acknowledgement.

An explicit pause starts a fresh release epoch on restart. A live replacement
preserves the epoch and previous-frame feedback history. Timeout before a
swap is claimed cancels it. A claimed-but-unacknowledged swap returns
`SESSION_EINPROGRESS` instead of freeing potentially referenced memory or
pretending the operation failed without effect.

The public manager and graph-control paths are guarded against bypassing this
ownership protocol. Third-party DSP cannot invoke the trusted control API.
The shell uses the actual managed control path, not raw graph writes.

## Persistent sessions

The versioned `# tessera-session v1` text format contains sample rate, frame
size, counter frequency, core count, plugin source paths, parameters, full
temporal contracts, affinity constraints, edges, and feedback flags. On load,
file-local indices are remapped to fresh PIDs. Rate, frame size and timebase
must match the current engine; there is no implicit conversion of budgets.

A complete replacement is validated/admitted and initialized before the live
scene is replaced. Missing files, malformed data, failed plugin initialization,
incompatible contracts and partial loads preserve the previous scene and
reclaim rejected candidate resources. Saves use the existing temporary-write
and rename VFS/FAT path. This is **not** a claim of power-loss-safe filesystem
transactions. The FAT backend is root-directory, 8.3-name storage.

The QEMU application uses an in-memory FAT block image containing real ELF
files. The serial acceptance runner exports that image, starts a completely
new emulator process, imports only the saved storage, loads the patch, and
compares actual PCM samples/hash. It does not preserve process memory across
boots. Physical EMMC2/SD and I2S acceptance belong to M10.

## Acceptance and remaining hardware work

```sh
make -j1 test-arm-m11-m13 CROSS_COMPILE=aarch64-linux-gnu-
make test-arm-session-console CROSS_COMPILE=aarch64-linux-gnu-
```

The combined gate builds the kernel, runs generated multicore admission and
pthread execution tests, malformed-input/serialization tests, live-session
concurrency tests, existing shell/patch regressions, actual four-core ELF
capacity/fault/transport acceptance, and the serial two-boot persistence test.

The capacity fixture uses an explicitly declared 12 kHz / 240-sample
(20 ms) emulation profile. The identical probe runs serially for more than
one frame, is rejected on one core and admitted on three. Its
three timer-bound DSP probes each occupy 40% of the frame, and declare a 60%
budget including emulated entry overhead. The same probes run longer than one
frame serially, are rejected on one DSP core, and are accepted on three. This
is a scheduling/containment demonstration, not a fixed-instruction throughput
benchmark or Cortex-A72 WCET measurement. The intentionally overloaded serial
measurement has its cadence stopped; the parallel measurement starts with a
fresh cadence rather than inheriting the benchmark's delayed IRQs.

The interactive application defaults to 48 kHz / 64-sample (750 Hz) frames.
The automated serial correctness/cold-boot gate builds it at 48 kHz /
240-sample (200 Hz) frames, keeping the exact zero-missing-frame and watchdog
assertions. `test-arm-session-console-fast` retains the strict 750 Hz
wall-clock diagnostic. This is a separate, lower-rate functional acceptance
profile, not a fix or a passing result for the previously failing cold-entry
750 Hz stress runs.
Actual wall-clock deadline/overrun counters remain observable; passing a
finite QEMU run is not a guarantee under arbitrary host contention. Earlier
failed runs are kept in the verification evidence. The unrelated legacy M12
wall-clock fixtures remain a separate timing-sensitive regression boundary.

Physical audio, DMA/cache-coherency validation, hardware WCET and jitter under
memory pressure, and a recorded CM4 fault-containment demonstration remain M10
work. The board's existing boot entry is still the bring-up/self-test image;
`run-arm-workstation` is the runnable QEMU application, not a claim that the
board boot has already been wired to the new session engine.


The singleton scheduler adopts a same-PID update without copying/clearing the
full 16-task state table. A regression verifies that two accumulated strikes,
a third-strike kill after a budget update, and the killed state survive updates;
a new PID still starts with fresh state. Idle cadence/control observers park
rather than busy-spin, and the cadence publishes a wake event after its frame
counter. These are overhead fixes, not evidence that 750 Hz cold-start timing
is established on every emulator host.

The serial functional profile explicitly uses 800 us budgets and 4,000/4,800 us
deadlines in its 5 ms frame; it requires zero plugin budget/deadline failures
as well as zero missing output blocks/watchdog overruns during the checked
live edits and restored run. The fast diagnostic retains its separate 400 us,
1,000/1,200 us contracts for the 1.333 ms frame. The profile and counters are
saved in each result JSON, avoiding ambiguous comparisons between the two.
