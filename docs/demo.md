# M8 resilience demo: malicious plugin caught, killed, audio keeps running

This is the M8 milestone's "done when" demo (issue #36):

> An externally-supplied plugin binary is loaded at runtime, sandboxed, and
> crashing it does not disturb the audio engine or other plugins.

Three plugins are loaded into isolated, sandboxed address spaces and wired into
the audio graph:

| Plugin | Source | Behaviour |
| --- | --- | --- |
| `good`  | [`plugins/test/good_plugin.c`](../plugins/test/good_plugin.c)   | A clean 440 Hz sine generator. Renders audio every block. |
| `crash` | [`plugins/test/crash_plugin.c`](../plugins/test/crash_plugin.c) | Dereferences a NULL pointer inside `process_block`. |
| `evil`  | [`plugins/test/evil_plugin.c`](../plugins/test/evil_plugin.c)   | Issues a syscall (`SVC`) from `process_block`, then attempts a wild write to a kernel address. |

Every block, the `good` plugin's `process_block` runs and the host (the "DAC")
reads real audio back from its output. At the trigger block - modelling "after
3 seconds" - the `crash` and `evil` plugins are run and are each caught and
killed. The `good` plugin and the DAC never miss a block. The whole load / run
/ kill / unload cycle repeats 10 times and the frame allocator returns exactly
to its baseline, so nothing leaks.

Two independent containment mechanisms are exercised:

- **MMU data abort** - `crash`'s NULL dereference (and `evil`'s wild kernel
  write) fault from EL0; the MMU blocks the access and the kernel kills the
  process. Kernel memory is never touched.
- **Syscall gate** - `evil`'s `SVC` from the audio path is a protocol violation
  (a sandboxed plugin may only reach the kernel through its controlled
  trampoline, issue #35), so the kernel kills it instead of servicing the call.

## Run it

The demo runs on the QEMU `virt` board (MMU on, real exception vectors,
isolated EL0 plugins) and is verified in CI:

```
make test-arm-resilience-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

It builds the three plugins as standalone AArch64 ELFs, boots the kernel demo
harness ([`tests/arm64/virt/resilience_main.c`](../tests/arm64/virt/resilience_main.c)),
and asserts `RESILIENCE: PASS`.

## Transcript

A representative run (the fault banners repeat once per iteration, one for the
`crash` data abort and one for the `evil` illegal-SVC kill):

```
=== QEMU virt M8 resilience demo (issue #36) ===

[exception] memory fault
  vector : EL0 sync
  cause  : data abort (EL0) (EC=0x24)
  ESR=0x0000000092000046  ELR=0x0000008000000028
  FAR=0x0000000000000000  SPSR=0x00000000000003c0
  -> terminating EL0 process
  [fault] terminating EL0 process pid=2 (crash) on write access
  [sandbox] illegal SVC #1 from pid=3 (evil)
  run 1: good-audio-intact + both-killed = yes
  ...
  run 10: good-audio-intact + both-killed = yes
leak: baseline=32151 after-10x=32151 no-leak=1
checks: passes=10/10 no-leak=1
RESILIENCE: PASS
```

Every iteration: `good` produced audio on all 6 blocks (no dropout), both
hostile plugins were killed with logged fault info, and the free-page count is
unchanged after 10 iterations.

## Hardware capture

The reproducible QEMU harness above is the CI-verified form of the demo. A
screen/audio capture of the same three plugins running on real Raspberry Pi
CM4 hardware - the good plugin audible throughout while the crash and evil
plugins are loaded and killed - belongs in `docs/demo/` (e.g.
`docs/demo/resilience-cm4.mp4`) and requires a board to record.
