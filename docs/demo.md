# Resilience demo: hostile plugins caught, killed, audio keeps running

This is the resilience "done when" demo. It began as the M8 capstone (issue
#36) and grew a third leg with M12's time-safe sandbox (issue #79):

> An externally-supplied plugin binary is loaded at runtime, sandboxed, and
> crashing - or hanging - it does not disturb the audio engine or other plugins.

Four plugins are loaded into isolated, sandboxed address spaces and wired into
the audio graph:

| Plugin | Source | Behaviour |
| --- | --- | --- |
| `good`  | [`plugins/test/good_plugin.c`](../plugins/test/good_plugin.c)   | A clean 440 Hz sine generator. Renders audio every block. |
| `crash` | [`plugins/test/crash_plugin.c`](../plugins/test/crash_plugin.c) | Dereferences a NULL pointer inside `process_block`. |
| `evil`  | [`plugins/test/evil_plugin.c`](../plugins/test/evil_plugin.c)   | Issues a syscall (`SVC`) from `process_block`, then attempts a wild write to a kernel address. |
| `hog`   | [`plugins/test/hog_plugin.c`](../plugins/test/hog_plugin.c)     | Spins forever inside `process_block` - no bad access, no syscall, just stolen time. |

Every block, the `good` plugin's `process_block` runs and the host (the "DAC")
reads real audio back from its output. At the trigger block - modelling "after
3 seconds" - the `crash` and `evil` plugins are run and are each caught and
killed, and the `hog` starts running under its CPU budget: it is preempted at
its budget boundary every block, muted while it offends, and killed by the
escalation policy on its third consecutive offence. The `good` plugin and the
DAC never miss a block. The whole load / run / kill / unload cycle repeats 10
times and the frame allocator returns exactly to its baseline, so nothing leaks
- including the budget-killed `hog`.

## The three sandbox legs

A plugin can misbehave in exactly three ways, and each has an independent,
hardware- or kernel-enforced containment:

- **Memory - MMU data abort.** `crash`'s NULL dereference (and `evil`'s wild
  kernel write) fault from EL0; the MMU blocks the access and the kernel kills
  the process. Kernel memory is never touched.
- **Syscalls - the SVC gate.** `evil`'s `SVC` from the audio path is a protocol
  violation (a sandboxed plugin may only reach the kernel through its controlled
  trampoline, issue #35), so the kernel kills it instead of servicing the call.
- **Time - the CPU budget.** `hog` commits no bad access and makes no syscall,
  so neither of the above can catch it - it simply never returns. The kernel's
  budget timer preempts it at its budget boundary mid-`process_block` (issue
  #78); the host mutes it, and after three consecutive offences kills it. This
  is the leg that makes an *untrusted* plugin safe on the availability axis, not
  just the memory-safety axis.

## Run it

The demo runs on the QEMU `virt` board (MMU on, real exception vectors,
isolated EL0 plugins) and is verified in CI:

```
make test-arm-resilience-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

It builds the four plugins as standalone AArch64 ELFs, boots the kernel demo
harness ([`tests/arm64/virt/resilience_main.c`](../tests/arm64/virt/resilience_main.c)),
and asserts `RESILIENCE: PASS`.

## Transcript

A representative run. Each iteration logs one banner per containment: the
`crash` data abort, the `evil` illegal-SVC kill, and the `hog`'s three budget
preemptions ending in a budget kill.

```
=== QEMU virt resilience demo (issue #36 + #79) ===
hog budget: 1333us of a 5333us block (fair share of 4)

[exception] memory fault
  vector : EL0 sync
  cause  : data abort (EL0) (EC=0x24)
  ESR=0x0000000092000046  ELR=0x0000008000000028
  FAR=0x0000000000000000  SPSR=0x00000000000003c0
  -> terminating EL0 process
  [fault] terminating EL0 process pid=2 (crash) on write access
  [sandbox] illegal SVC #1 from pid=3 (evil)
  [budget] preempting EL0 process pid=4 (hog) at its budget boundary
  [budget] preempting EL0 process pid=4 (hog) at its budget boundary
  [budget] preempting EL0 process pid=4 (hog) at its budget boundary
  [budget] kill pid=4 (hog) after 3 consecutive offences (last=1481us budget=1333us)
  run 1: good-audio-intact + all-three-neutralised = yes
  ...
  run 10: good-audio-intact + all-three-neutralised = yes
leak: baseline=32125 after-10x=32125 no-leak=1
checks: passes=10/10 no-leak=1
RESILIENCE: PASS
```

Every iteration: `good` produced audio on all 6 blocks (no dropout); the `crash`
and `evil` plugins were killed with logged fault info; the `hog` was preempted
at its budget boundary on each of its three blocks (each preempted run lands
between the 1333 us budget and the block period, proving the timer - not a
voluntary return - stopped it), muted twice, and killed on the third offence;
and the free-page count is unchanged after 10 iterations.

## Hardware capture

The reproducible QEMU harness above is the CI-verified form of the demo. A
screen/audio capture of the same four plugins running on real Raspberry Pi
CM4 hardware - the good plugin audible throughout while the crash, evil, and
hog plugins are loaded and neutralised - belongs in `docs/demo/` (e.g.
`docs/demo/resilience-cm4.mp4`) and requires a board to record.
