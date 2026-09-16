# Tessera

**A bare-metal AArch64 audio platform for running third-party DSP plugins as isolated processes.**

Tessera is built around one idea: loading an audio plugin should not mean trusting it
with the whole instrument. Each plugin runs at EL0 in its own ARMv8 virtual address
space, with host-enforced CPU budgets and a syscall gate. A bad pointer, an illegal
syscall, or an infinite DSP loop is contained to that plugin instead of taking down
the audio engine.

The result is a small real-time audio OS with a plugin SDK, multicore graph scheduler,
serial workstation, persistent patches, MIDI/transport support, and a growing DSP
library—all without Linux underneath the runtime.

> **Current status:** the software platform is extensively verified on QEMU `virt`,
> including real EL0 plugin execution, MMU faults, budget preemption, multicore DSP,
> graph mutation, persistence, and serial control. **Physical CM4/Pi 4 acceptance is still incomplete**: real BCM2711 boot/SD,
> I2S+DMA, and measured hardware latency are not yet claimed complete.

---

## Why Tessera exists

A conventional native audio host usually shares one process or one kernel with all
plugin code. Tessera instead treats a plugin as untrusted code and gives it explicit
resource boundaries:

- **Memory isolation** — a separate translation table per plugin; kernel memory,
  MMIO, and other plugins are unmapped.
- **Time isolation** — `process_block` executes under a generic-timer budget; a DSP
  loop can be interrupted, muted, and eventually killed.
- **Syscall isolation** — plugin-body `SVC` is forbidden; the host controls every
  kernel transition.
- **Memory and I/O quotas** — plugin footprint and syscall/I/O rate can be bounded
  independently of CPU time.
- **Failure containment** — MMU faults, illegal syscalls, and CPU-budget violations
  terminate only the offending plugin; graph fallback can keep audio flowing.

That makes isolation part of the audio architecture rather than an add-on around a
single trusted process.

---

## What works today

### Isolated plugin runtime

Tessera loads self-contained AArch64 ELF plugins into separate EL0 address spaces.
The host validates the image and ABI, maps code/data with the correct permissions,
and enters plugin callbacks through a controlled trampoline. The plugin ABI is stable
at major version 1 and currently includes backward-compatible event/transport
extensions through **v1.3**.

The runtime includes:

- load / unload / parameter updates at run time;
- MMU fault containment and plugin liveness publication;
- timer-enforced per-plugin CPU budgets and escalation policies;
- memory-footprint quotas and syscall/I/O-rate quotas;
- bounded lifecycle calls and rollback on failed load/admission;
- safe reclamation only after workers have drained.

See [`docs/plugin-abi.md`](docs/plugin-abi.md) and
[`docs/temporal-contracts.md`](docs/temporal-contracts.md).

### Real-time graph and multicore scheduling

CPU0 owns the audio cadence while isolated DSP jobs run on worker cores. The temporal
runtime admits a graph before execution using per-plugin periods, deadlines, budgets,
criticality, dependencies, and optional core affinity. Same-frame cross-core edges use
bounded handoff rather than allowing a worker to stall the audio core.

Implemented pieces include:

- dependency-aware CPU1-3 placement and per-core execution;
- HARD / SOFT / BEST_EFFORT temporal contracts;
- mute, bypass, kill, strike-based kill, and degrade policies;
- frame-boundary graph replacement with guarded reclamation;
- feedback edges and plugin delay compensation;
- per-plugin runtime counters and service-time snapshots;
- live reconfiguration without doing ELF loading or filesystem work in the audio IRQ.

The strict multicore QEMU fixture proves that work which overruns one worker can execute
across CPU1-3 while CPU0 keeps cadence, and exercises crash/hog containment on every
worker core.

See [`docs/temporal-contracts.md`](docs/temporal-contracts.md) and
[`docs/shell.md`](docs/shell.md).

### Interactive workstation

Tessera can be driven from a serial terminal rather than from a compiled control
program. The managed shell exposes graph, scheduling, and persistence operations:

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

`patch load` reconstructs plugin paths, parameters, contracts, affinity, and wiring.
The serial-workstation acceptance test saves a session to a FAT image, terminates QEMU, boots a new
emulator process with only that storage restored, reloads the patch, and requires
bit-identical PCM output.

See [`docs/shell.md`](docs/shell.md).
### Plugin SDK and DSP library

Third-party plugins need only the self-contained [`sdk/`](sdk/) directory and a stock
AArch64 toolchain. The SDK provides the ABI header, link script, static helper library,
reference plugin, C build template, and a Rust wrapper.

`libtessera.a` now includes real-time-safe, allocation-free building blocks such as:

- smoothers, biquads, SVFs, ADSRs, delay lines, envelope followers;
- polyBLEP, wavetable, and FM oscillators;
- streaming sample playback and a polyphonic synth voice engine;
- compressor, EQ, gate, chorus, delay, overdrive, and FDN reverb;
- FFT/rFFT, partitioned convolution, phase vocoder/pitch shift, spectrum analysis;
- modulation matrix, MPE helpers, and sample-accurate event splitting;
- fixed-memory sample-rate conversion and other utility DSP.

The point is not to turn the kernel into a DAW. These live in the SDK so plugin authors
can build useful instruments and effects without giving up the isolation model.

Start with [`docs/getting-started.md`](docs/getting-started.md) or
[`sdk/README.md`](sdk/README.md).

### Reliability and performance features

The repository also contains higher-level reliability work built on top of the sandbox:

- safe-mode dry bypass when an effect dies;
- glitch-free crossfaded patch switching;
- isolated plugin hot-reload with no silent block;
- persistent crash black-box recording;
- signed plugin packages and revocation support;
- secure/measured-boot primitives;
- parser fuzzing, golden-audio regression, chaos-mode fault injection, and TSan queue checks.

These mechanisms are useful because a contained failure leaves enough of the system
alive to recover, bypass, report, or replace the failed component.

See [`docs/reliability.md`](docs/reliability.md) and [`docs/demo.md`](docs/demo.md).

### Musical control and audio I/O model

Tessera includes MIDI/CV control paths, a master musical transport, tempo sync,
arpeggiation, MPE/per-note expression, sample-accurate events, scene morphing, looping,
audio-input graph nodes, multi-channel configuration, USB-audio support, and software
sample-rate conversion. QEMU harnesses exercise these data paths; the corresponding
physical CM4 audio-I/O acceptance is still pending.

---

## Architecture

```text
                    trusted kernel / host

        audio cadence + graph ownership + admission
                         CPU0
                          |
                  frame-boundary kick
                 /         |         \
             worker      worker      worker/control
              CPU1        CPU2          CPU3
                |           |
             EL0 PID A   EL0 PID B
            +---------+ +---------+
            | private | | private |
            | VA space| | VA space|
            +---------+ +---------+
                \           /
             trusted audio buffers
                      |
                    DAC sink
```

Each plugin gets its own translation root and cannot address another plugin or kernel
memory. Audio/control buffers are explicitly mapped by the host. The kernel owns
lifecycle, graph publication, timer enforcement, and fault handling; untrusted plugin
code never becomes privileged.

---

## Quick start

### Build the AArch64 image

LLVM is the default cross-toolchain:

```sh
make arm-install-deps   # Ubuntu/Debian: clang, lld, llvm, binutils
make arm
```

This produces:

```text
build/arm/kernel8.elf
build/arm/kernel8.img
```

A GNU cross-toolchain works as well:

```sh
make arm CROSS_COMPILE=aarch64-linux-gnu-
# or: CROSS_COMPILE=aarch64-none-elf-
```

See [`docs/build-arm.md`](docs/build-arm.md) for the bare-metal image and CM4 boot
layout.

### Run a plugin under QEMU

Install QEMU and the packaged GNU AArch64 toolchain, then run the SDK acceptance:

```sh
sudo apt-get install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu qemu-system-arm
make test-arm-sdk-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

The test boots Tessera on QEMU `virt`, loads a real plugin ELF into an isolated EL0
process, runs its DSP callback, and verifies audio plus parameter control.

### Run the serial workstation

```sh
make run-arm-workstation CROSS_COMPILE=aarch64-linux-gnu-
```

From the QEMU serial console, use `help`, `load`, `wire`, `set-param`, `contract`,
`pin`, `stats`, `ls`, `patch save`, and `patch load` to build and manage a running
patch without recompiling a control program.

---

## Verification

Useful feature-level gates include:

```sh
make -j1 test-arm-temporal-all CROSS_COMPILE=aarch64-linux-gnu-
make -j1 test-arm-resilience-qemu CROSS_COMPILE=aarch64-linux-gnu-
make -j1 test-arm-shell-qemu CROSS_COMPILE=aarch64-linux-gnu-
make -j1 test-arm-shell-graph-qemu CROSS_COMPILE=aarch64-linux-gnu-
make -j1 test-arm-shell-patch-qemu CROSS_COMPILE=aarch64-linux-gnu-
make -j1 test-arm-session-console CROSS_COMPILE=aarch64-linux-gnu-
```

| Gate | What it proves |
| --- | --- |
| `test-arm-temporal-all` | temporal contracts, admission, lifecycle races, timer enforcement, real-EL0 execution |
| `test-arm-resilience-qemu` | MMU / kernel-write / illegal-SVC / CPU-hog containment with leak checks |
| `test-arm-shell-qemu` | serial shell safety and shared-UART behavior |
| `test-arm-shell-graph-qemu` | console graph construction and real plugin audio |
| `test-arm-shell-patch-qemu` | console patch save/reload and identical output |
| `test-arm-session-console` | managed multicore serial session, persistence, and true two-process cold boot |

Host tests use sanitizers extensively, and dedicated race tests exercise the lock-free
queues under ThreadSanitizer.

Some QEMU scheduling tests intentionally use a longer emulation frame than the product
48 kHz profile so host-vCPU descheduling is not mistaken for Cortex-A72 WCET evidence.
The assertions on missing/skipped frames, faults, graph state, and PCM correctness remain
strict. Physical timing numbers must come from the board.

---

## Project status

The software platform is broadly complete and heavily exercised under QEMU, including
audio capture/input/round-trip coverage, multicore scheduling, temporal enforcement,
persistent sessions, and the current SDK/DSP stack.

The major unfinished work is **proving the platform on real BCM2711 hardware**.
The remaining tracked hardware/integration work is:

- [#105](https://github.com/KrasForge/Tessera/issues/105) — complete Pi 4 / CM4 boot,
  interrupt, serial, mailbox, and EMMC2/SD bring-up on real silicon;
- [#106](https://github.com/KrasForge/Tessera/issues/106) — real 48 kHz I2S + DMA output
  to the PCM5102 with sustained zero-underrun playback;
- [#107](https://github.com/KrasForge/Tessera/issues/107) — make QEMU `raspi4b` a required
  BCM2711-path CI gate;
- [#108](https://github.com/KrasForge/Tessera/issues/108) — publish CM4 latency/jitter
  measurements and record the physical fault-containment demo.

Until those are done, Tessera should be read as a **working and heavily tested bare-metal
audio architecture in emulation**, not as a finished CM4 product or a claim of measured
hardware real-time performance.

See the `docs/` directory for design notes, acceptance evidence, and hardware plans.

---

## Repository map

```text
arch/arm64/   kernel architecture, MMU/processes, graph/runtime, scheduling
boot/         AArch64 boot entry and Pi boot configuration
drivers/      UART, GIC, timer-facing hardware support, I2S/DMA/platform I/O
include/      public kernel/plugin ABI headers
plugins/      reference and adversarial test plugins
sdk/          standalone plugin SDK, DSP library, examples, Rust wrapper
scripts/      QEMU timing and workstation acceptance runners
tests/        host, sanitizer, concurrency, and QEMU integration tests
tools/        offline/developer tooling
docs/         ABI, hardware, shell, reliability, timing, and design references
```

---

## Documentation

- **Write a plugin:** [`docs/getting-started.md`](docs/getting-started.md)
- **Plugin contract:** [`docs/plugin-abi.md`](docs/plugin-abi.md)
- **SDK and DSP blocks:** [`sdk/README.md`](sdk/README.md)
- **Serial workstation:** [`docs/shell.md`](docs/shell.md)
- **Temporal contracts:** [`docs/temporal-contracts.md`](docs/temporal-contracts.md)
- **Fault containment demo:** [`docs/demo.md`](docs/demo.md)
- **Reliability mechanisms:** [`docs/reliability.md`](docs/reliability.md)
- **Latency methodology:** [`docs/latency.md`](docs/latency.md)
- **Hardware wiring/targets:** [`docs/hardware.md`](docs/hardware.md), [`docs/hardware-targets.md`](docs/hardware-targets.md)

---

## Origin

Tessera began as a fork of [IKOS](https://github.com/Ikey168/ikos). The old x86 teaching-OS
tree has since been removed from the active source tree; its history remains in Git. What
survived conceptually is the small-kernel/process-isolation approach. The hardware layer,
audio engine, AArch64 process/runtime code, plugin platform, scheduler, and SDK are the
Tessera-specific system built on top of that starting point.

---

## License

MIT. See [`LICENSE`](LICENSE).
