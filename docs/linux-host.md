# Tessera host on Linux

A proposal for porting the **host model** - the plugin ABI, per-plugin
isolation, the syscall gate, CPU budgets, and the never-go-silent semantics -
to userspace Linux, as a sibling deliverable to the microkernel. Like
[`docs/hardware-targets.md`](hardware-targets.md) and
[`docs/feature-ideas.md`](feature-ideas.md), this is a place to draw from, not
a commitment.

---

## Why

Tessera's thesis is that untrusted plugins must be fault-contained: a plugin
that crashes, hangs, or turns hostile cannot take the audio engine with it.
The microkernel proves that thesis with the strongest possible mechanism -
MMU-isolated processes on bare metal. But the mechanism and the thesis are
separable, and the thesis also holds on a platform people already run.

Every user Tessera could plausibly reach today is on Linux: Elk runs plugins
inside Sushi (one process, one address space), MOD runs LV2 plugins inside
`mod-host` (same), and both inherit the consequence - one bad plugin can
corrupt or kill the whole engine. Linux *processes*, however, are isolated
address spaces too. A host that runs each untrusted plugin as its own
real-time Linux process, wired with the same shared-memory rings and governed
by the same budgets and gate policy Tessera already implements, carries the
never-go-silent guarantee to stock hardware - a plain Pi 4 with a HiFiBerry
hat - with every driver, filesystem, and toolchain inherited for free.

This is not a replacement for the kernel. It is a second implementation of
the same host model, and the two make each other stronger:

- The **Linux host** is the reach: it runs where users are, today, and turns
  the isolation thesis into something anyone can `apt install` next to their
  existing setup.
- The **microkernel** is the proof: the reference implementation whose
  worst-case behaviour can actually be bounded, against which the Linux
  host's tail latencies are measured (see
  [the comparison](#lh3---the-comparison) below).

M10 - real hardware bring-up - remains the kernel's gating milestone and is
unaffected by any of this.

---

## What "the host model" means

The portable intellectual property, independent of the kernel underneath:

1. **The plugin ABI** ([`docs/plugin-abi.md`](plugin-abi.md)): five C exports,
   fixed block size, wait-free `set_param`, the sandbox contract of §5 and the
   real-time constraints of §6. The ABI already says a plugin may touch only
   its own memory and the buffers the host hands it - it never promises *how*
   that is enforced.
2. **Per-plugin address-space isolation**, with fault containment: a plugin
   that faults is killed; nothing else is disturbed.
3. **The syscall gate**: a plugin on the audio path that attempts I/O or any
   un-granted syscall is killed (M8, `sandbox.c`).
4. **Resource budgets**: per-plugin CPU time per block (M12, `budget.c`) and
   syscall/I/O-rate quotas (M22, `io_quota.c`); repeated violation
   neutralizes the plugin.
5. **Never-go-silent semantics**: safe-mode bypass (`safe_bypass.c`), crash
   black-box (`blackbox.c`), hot reload (`hot_reload.c`), glitch-free patch
   switching (`xfade.c`) - the theme-A features of
   [`docs/feature-ideas.md`](feature-ideas.md).
6. **The graph**: nodes, shared-memory ring edges, seqlock rewire, topology-
   aware scheduling (`audio_graph.c`, `graph_sched.c`).

All of it was written to be mechanism-light: plain C, lock-free SPSC rings,
integer accounting, no floating point in the control plane. That is why the
port is cheap.

---

## The mechanism mapping

| Host-model piece | Tessera kernel mechanism | Linux mechanism |
|---|---|---|
| Plugin address space | `process_create`, per-process translation root (`vmem.c`, `mmu.c`) | one process per plugin (`fork`/`exec` of a runner) |
| Plugin binary | static EL0 ELF, `elf64.c` loader + `plugin_loader.c` validation | the same five exports built as a PIE `.so`, `dlopen`ed by the runner |
| Syscall gate | EL0 + SVC gate; raw syscall on the audio path → kill (`sandbox.c`) | seccomp-BPF allowlist installed after `plugin_init`, `SECCOMP_RET_KILL_PROCESS` on violation |
| Fault containment | data abort at EL0 → process killed, system continues (`exceptions.c`) | `SIGSEGV` kills the runner; host observes via `pidfd`/`waitpid`, engages safe-mode bypass |
| Audio edges | shared-memory rings mapped into both address spaces, zero syscalls per block (`audio_ringbuf.c`, `spsc_ring.c`) | the **same ring code** over `memfd_create` + `mmap` shared mappings |
| Parameter delivery | per-plugin lock-free queue mapped into the plugin (`param_queue.c`) | same code, same shared mapping |
| Real-time cadence | audio thread pinned to CPU0, generic timer, overrun watchdog (`audio_core.c`) | `SCHED_FIFO` + `mlockall` + CPU isolation (`isolcpus`/cpusets) on a PREEMPT_RT kernel (mainline since 6.12) |
| CPU budget | per-block cycle accounting via `CNTPCT_EL0` (`budget.c`) | the same accounting logic via `CLOCK_MONOTONIC_RAW`; cgroups only as a backstop |
| I/O quota | `io_quota.c` token buckets at the SVC gate | same buckets at the host's control-plane socket; `rlimits` behind them |
| Audio I/O | `drivers/i2s.c` + DMA to the PCM5102 | an ALSA (or JACK/PipeWire-client) source/sink node at the graph edge |
| Control plane | `SYS_PLUGIN_LOAD` / `SYS_GRAPH_CONNECT` … via `svc #0` ([`docs/syscalls.md`](syscalls.md)) | the same verbs over a Unix-domain socket to the host daemon |
| Shell | UART shell (`shell.c`, [`docs/shell.md`](shell.md)) | same shell over stdin / the socket |
| Storage | FAT on SD (`fat.c`, `vfs.c`) | the filesystem |
| Secure boot / signed plugins | `secureboot.c`, `package.c` + `sha256.c` | same package verification code, host-side |

Two rows deserve honesty:

- **"Zero syscalls per block" degrades.** In the kernel, graph edges cost no
  kernel involvement per block. On Linux, waking a plugin process each block
  costs a futex wake (~microseconds), or the runner busy-polls on an isolated
  core. One wake per plugin per block at a 1 kHz block rate is well within
  budget on an A72, but it is a real difference and belongs in the measured
  comparison, not under the rug.
- **The gate is coarser.** The SVC gate can distinguish "on the audio path"
  from "off it" per call site; seccomp cannot see application phase, so the
  Linux gate is two filters - a permissive one during `plugin_init`, a strict
  one sealed before the block loop starts (`prctl(PR_SET_NO_NEW_PRIVS)` +
  the final filter). Namespaces (user, pid, mount with an empty root, net)
  wrap the runner as belt and braces.

---

## What carries over, what gets replaced

In the spirit of the README's IKOS section - the years of work carry; the
hardware layer changes.

**Ports with little or no change** (plain C, no privileged instructions):
`audio_ringbuf.c`, `spsc_ring.c`, `param_queue.c`, `audio_graph.c`,
`graph_control.c`, `graph_sched.c`, `budget.c` (accounting core),
`io_quota.c`, `safe_bypass.c`, `blackbox.c`, `xfade.c`, `bank.c`, `patch.c`,
`presets.c`, `ctlmap.c`, `tempo_sync.c`, `transport.c`, `mixer.c`,
`limiter.c`, `src.c`/`src_fir.c`, `package.c`, `sha256.c`, `shell.c`, and the
entire SDK ([`sdk/`](../sdk/)) - the same `plugin_abi.h`, rebuilt as a PIE.
The host-side test suite in [`tests/arm64/`](../tests/arm64/) already runs
most of this on the build host; that is the porting head start.

**Replaced by Linux equivalents:** `vmem.c`/`mmu.c`/`pmm.c` (the MM),
`sched.c`/`runqueue.c`/`smp.c` (the scheduler), `exceptions.c`/`vectors.S`
(signals), `elf64.c` (the dynamic linker), `syscalls.c` (the socket
protocol), everything in [`drivers/`](../drivers/) (ALSA/evdev), `fat.c`/
`vfs.c` (the filesystem), `secureboot.c` (out of scope on stock Linux).

**New, small:** the runner (a few hundred lines: `dlopen`, map the shared
regions, drop into the sandbox, run the block loop), the host daemon's
process supervision (`pidfd`-based), and the ALSA edge node.

---

## Architecture sketch

```
                    tessera-hostd  (SCHED_FIFO, mlockall)
                    graph manager · budgets · supervision · shell socket
                          |                       |
              memfd rings + param queues     pidfd / signals
                   /              \
   plugin runner (proc A)     plugin runner (proc B)      alsa-io node
   seccomp-sealed, no fs      seccomp-sealed, no fs       (the only proc
   dlopen(synth.so)           dlopen(reverb.so)            that touches
   block loop on ring         block loop on ring           the device)
```

The graph, the rings, the budgets, and the failure semantics are the ones
Tessera already has; only the enforcement substrate changes.

---

## What this unlocks beyond reach

- **A desktop dev loop.** The ABI is architecture-neutral C; runners build
  for `x86_64` too. Plugin authors get edit-compile-run on a laptop with real
  isolation semantics - and the "offline plugin host" from
  [`docs/feature-ideas.md`](feature-ideas.md) §G falls out as the runner with
  a WAV node instead of ALSA.
- **An apples-to-apples measurement.** Same CM4, same plugins, same graph:
  Tessera kernel vs. Linux host. Jitter, wakeup latency, overruns, kill
  latency for `crash`/`evil`/`hog`. Nobody has published that comparison;
  it is the strongest possible argument *for* the microkernel wherever the
  numbers favour it, and honest scoping wherever they do not.
- **A CLAP on-ramp.** The runner is the natural place for a
  [CLAP](https://cleveraudio.org/) shim: a runner variant that hosts a CLAP
  plugin behind the same rings and budgets would let existing open-source
  plugins run fault-contained without adopting the Tessera ABI first.

---

## Milestones

Numbered `LH` to keep the kernel's M-series untouched.

### LH0 - host skeleton

`tessera-hostd` plus the runner on desktop Linux: load the SDK example
plugins as `.so`s, wire a graph over memfd rings, output via ALSA (or a WAV
sink in CI).

**Done when:** `example_sine` plays while `example_crasher` is loaded,
faults, and is reaped - audio never stops, the host never restarts, and the
existing golden-output tests pass against the Linux host.

### LH1 - the gate and the budgets

Seal the runners with the two-phase seccomp filter, namespaces, and rlimits;
port the M12 CPU budget and M22 I/O quota accounting.

**Done when:** the M8 resilience demo runs unchanged on Linux - `crash`,
`evil` (raw syscall on the audio path), and `hog` (infinite loop) are each
detected and killed within a bounded number of blocks while the good plugin
never misses one, over 10 cycles with no leaks.

### LH2 - stock hardware

A plain Raspberry Pi 4 with a HiFiBerry-class DAC hat and a PREEMPT_RT
kernel: pinned audio thread, measured cadence, the serial/socket shell.

**Done when:** the LH1 demo is audible on stock hardware someone could buy
this week, with round-trip latency and jitter published in
[`docs/latency.md`](latency.md).

### LH3 - the comparison

Dual-boot the same CM4 between the Tessera kernel and the Linux host; run
the same plugin set and measurement harness on both.

**Done when:** [`docs/latency.md`](latency.md) carries both columns - period
jitter, wakeup latency, overruns, and plugin-kill latency - and the README's
claims cite the measured delta instead of an architectural argument.

---

## Risks and open questions

- **Wakeup chains.** A deep graph serialized across processes pays one futex
  wake per hop per block; topology-aware batching (`graph_sched.c`'s job) and
  core placement matter more here than in the kernel. Budget for this in LH0,
  measure in LH3.
- **PREEMPT_RT tails.** Mainline RT is good (double-digit-microsecond typical
  worst cases on well-configured ARM boards) but not bounded; SMIs, drivers,
  and thermal events happen. This is precisely what LH3 exists to quantify -
  the risk is a finding, not a blocker.
- **`dlopen` vs. static ELF.** `dlopen` in the runner is the pragmatic
  choice, but it runs plugin constructors before the sandbox seals. Loading
  order (map → seal → resolve → init) needs care; the fallback is keeping the
  static-ELF loader and `mmap`ing segments by hand, as `elf64.c` does today.
- **Split focus.** The real cost is attention, not code. The mitigation is
  scope discipline: LH0-LH1 reuse the host-test layer that already exists,
  and nothing in the LH series touches the kernel tree.
