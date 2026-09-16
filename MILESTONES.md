# Tessera milestones

The roadmap is organised as milestones, each a small set of GitHub issues with
a concrete **"done when"** criterion - a demo or measurement, not a feature
list. Milestones M0-M9 (issues #1-#40) and M11-M13 (#74-#82) are complete in QEMU; this file records what
they delivered and defines the milestones ahead.

Everything below M10 is verified in CI on the QEMU `virt` board (MMU on, real
exception vectors, EL0 plugins). M10 exists because none of it has run on real
silicon yet.

---

## Completed

### M0 - ARM bare-metal bring-up (#1-#6)

AArch64 cross-toolchain and bare-metal build (`make arm` ->
`build/arm/kernel8.img`), boot to a C environment with working UART output.

### M1 - ARM virtual memory (#7-#10)

The IKOS VMM ported to ARMv8 translation tables: 4 KiB granule, kernel/user
split, per-address-space page-table roots, map/unmap/protect.

### M2 - isolated processes (#11-#15)

`process_create` with a per-process translation root (#11), AArch64 exception
vectors (#12), EL0 entry and the SVC syscall ABI (#13), fault containment - a
process that touches kernel memory is killed, the system keeps running (#14),
and context switch between isolated processes (#15).

### M3 - audio out (#16-#18)

I2S driver for an external PCM5102 DAC (#16), DMA ring-buffer streaming at
48 kHz / 16-bit (#17), and the sine-tone "audio hello world" (#18).

### M4 - real-time core (#19-#22)

GIC-400 and ARM generic timer (#19), real-time priority scheduler with
preemption (#20), the audio thread pinned to a dedicated core with guaranteed
cadence and an overrun watchdog (#21), and callback latency/jitter measurement
reported off-core over UART (#22). See [`docs/latency.md`](docs/latency.md).

### M5 - plugins (#23-#26)

The plugin ABI - `init` / `process_block` / `set_param` (#23), an ELF loader
that gives each plugin its own address space (#24), shared-memory audio ring
buffers with zero syscalls per block (#25), and a plugin host that reads the
ring to the DAC and survives a plugin crash (#26).

### M6 - audio graph (#27-#29)

Graph model with nodes and ring-buffer edges (#27), `wire`/`unwire`
control-plane syscalls (#28), and the reference stereo low-pass effect (#29).

### M7 - control and input (#30-#33)

The minimal control syscall set: load, unload, set-param, wire, unwire (#30),
MIDI input over DIN-5/UART3 (#31), CV/Gate input via GPIO and an SPI ADC
(#32) - see [`docs/hardware.md`](docs/hardware.md) - and live parameter changes
via IPC with no audio dropout (#33).

### M8 - untrusted plugins (#34-#36)

Plugin ELFs loaded from SD/FAT with stronger validation (#34), a sandbox audit
plus the SVC gate - a plugin that issues a raw syscall from the audio path is
killed (#35), and the resilience demo: hostile plugins caught and killed while
audio keeps running, no leaks over 10 cycles (#36). See
[`docs/demo.md`](docs/demo.md).

### M9 - platform v1 (#37-#40)

Plugin ABI v1.0 frozen and documented (#37,
[`docs/plugin-abi.md`](docs/plugin-abi.md)), the plugin SDK - header, static
lib, example, build template (#38, [`sdk/`](sdk/)), the getting-started guide
for third-party authors (#39,
[`docs/getting-started.md`](docs/getting-started.md)), and patch/preset
persistence to SD/ramdisk (#40).

### M11 - multi-core plugin scheduling (#74-#76)

**Completed and regression-tested on QEMU `virt`; not hardware acceptance.**
CPU0 owns the audio cadence while lock-free workers on CPU1-3 execute graph
nodes. Dependency/core-aware admission chooses placements before the block
path, graph changes publish only at drained frame boundaries, a late worker is
skipped and attributed without blocking CPU0, and an empty worker stays parked.

**Done-when evidence:** the shared-session M11 fixture runs three identical
real EL0 jobs at 8 ms each against a 20 ms emulation frame. The same 24 ms of
work overloads one worker, while the admitted CPU1/2/3 plan completes 129/129
jobs per plugin with zero missing/skipped frames, deadline/budget failures, or
CPU0 watchdog overruns. A real low-pass chain is PCM-bit-identical when split
across cores. Crash and CPU-hog plugins are killed on CPU1, CPU2 and CPU3 while
the survivor chain keeps producing, and allocator baselines are restored after
every fault cycle. The original #74 stalled-worker and #75 live-rewire QEMU
fixtures remain part of the aggregate gate.

Run `make -j1 test-arm-m11 CROSS_COMPILE=aarch64-linux-gnu-`. The acceptance
profile is deliberately a 12 kHz / 240-sample QEMU scheduling test; 48 kHz /
64-sample hardware latency/WCET remains M10 work.

### M12 - per-plugin CPU budget enforcement (#77-#79)

**Completed and regression-tested on QEMU `virt`; not hardware acceptance.**
Per-plugin time accounting and atomic seqlock snapshots report service time
and offences. The shared `budget_plugin_run` execution path preempts EL0 at
a finite CPU budget, erases partially written output on an offence, and
publishes process death on the third consecutive offence. Killed processes
cannot be re-entered; resources are reclaimed after workers drain. The host
control syscall sets 64-bit counter-cycle budgets, and unload clears them.

**Done-when evidence:** the `hog` plugin renders three blocks, then writes
partial output and spins. It is muted and killed within three offending
blocks; the resilience demo verifies eight good-plugin blocks per cycle and
no frame leaks after each of ten cycles. A separate two-core timer-driven
acceptance test requires 100/100 good-plugin blocks, zero underruns, zero
worker skips, and zero audio-watchdog overruns. All four fault mechanisms
(null access, wild kernel write, illegal SVC, CPU budget) are exercised.

Run `make test-arm-m12 CROSS_COMPILE=aarch64-linux-gnu-` for the aggregate
gate. See [`docs/demo.md`](docs/demo.md) for measured transcripts and
[`docs/plugin-abi.md`](docs/plugin-abi.md#host-enforcement-m12-issue-78) for
integration constraints. M10's physical I2S/latency measurements remain open.

### M13 - interactive control shell (#80-#82)

**Completed and regression-tested on QEMU `virt`; not hardware acceptance.**
The fixed-buffer UART shell provides line editing, tokenisation, `help`, safe
error handling and fuzz-tested malformed-input containment. The graph/session
commands expose load/unload, wire/unwire, live parameters, graph/parameter
listing, runtime stats, temporal contracts/core placement, and persistent
session save/load/list operations without recompiling a control program.

**Done-when evidence:** the original #80 fixture runs the shell on CPU1 while
CPU2 emits periodic `audio_latency:` records through the shared UART and CPU0
records zero overruns. The #81 fixture builds `synth -> filter -> DAC` entirely
from console input and produces real plugin audio. The #82 fixture saves the
patch, tears down the graph, reloads it from FAT and reproduces the exact DAC
hash. The production workstation additionally exports only the FAT storage,
exits QEMU, boots a new emulator process, reloads the saved session and requires
bit-identical PCM. Malformed/missing/partially loadable sessions emit exactly
one error line, retain the old graph/audio, and `ls` reports live parameter
values while `patch ls` filters out plugin ELF files.

Run `make -j1 test-arm-m13 CROSS_COMPILE=aarch64-linux-gnu-`. The production
cold-boot acceptance uses a 12 kHz / 240-sample (20 ms) QEMU functional profile
with all zero-miss/zero-offence assertions intact; the 48 kHz / 64-sample stress
profile and physical serial/EMMC2/SD/I2S timing validation remain M10 work.

---

### M12 extension — admitted temporal contracts

The kernel also provides a frame-synchronous temporal host with independent
per-plugin block-multiple periods, within-frame deadlines, HARD/SOFT/BEST_EFFORT
classes, precedence-constrained EDF within each class, five configurable output
policies, and transactional graph admission. Both relative budgets and absolute
cutoffs interrupt actual EL0 calls. See
[`docs/temporal-contracts.md`](docs/temporal-contracts.md) for the supported
model, integration contract and limits; run `make test-arm-temporal-all`.
This does not imply arbitrary-release EDF, adaptive DSP quality, gapless
live graph rewiring or physical-hardware acceptance. The managed integration
below adds concurrent independent EL0 hosts and safe paused topology changes.


## M11/M13 integrated software delivery

The multicore temporal scheduler and interactive serial workstation are now
implemented together. Core/dependency-aware admission, isolated workers,
frame-boundary live graph replacement, guarded reclamation, contract/parameter
commands, and versioned persistent sessions share one execution engine.
`make test-arm-m11` and `make test-arm-m13` are the dedicated gates;
`make test-arm-m11-m13` remains the combined software gate.
`make run-arm-workstation` starts the serial application. See
[`docs/m11-m13-workstation.md`](docs/m11-m13-workstation.md) for commands,
verification scope, and the distinction from physical CM4/I2S acceptance.
Hardware integration is not implied by the software delivery.

## Next up

### M10 - real hardware bring-up (Raspberry Pi 4 / CM4)

Everything above runs on QEMU `virt`. The project's thesis - MMU-isolated
plugins on Cortex-A - has to be demonstrated on the BCM2711 before anything
else matters.

Scope:

- Boot on the Pi 4 / CM4: BCM2711 interrupt routing, PL011/mini-UART console,
  mailbox/clock setup, SD access via EMMC2.
- Real I2S + DMA to the PCM5102 at 48 kHz (the BCM2711 DMA controller replaces
  the virt test harness).
- Fill in the CM4 table in [`docs/latency.md`](docs/latency.md), idle and under
  load on CPU1-3.
- Record the M8 resilience demo on hardware
  (`docs/demo/resilience-cm4.mp4`): the good plugin audible throughout while
  the crash and evil plugins are loaded and killed.
- Intermediate, no board required: QEMU >= 9.0 provides a `raspi4b` machine;
  adding it to CI exercises the BCM2711 code paths before hardware does.

**Done when:** the latency table shows max jitter under 500 us with zero
overruns on an otherwise-idle CM4, and the resilience demo is captured on the
same board.

For the broader landscape of boards Tessera could target beyond the CM4 - the
emulatable-now options (Pi 3, Xilinx ZynqMP, SBSA), the product-grade audio
SoCs (i.MX8M, Rockchip), and the HAL / device-tree work that makes new boards
cheap - see [`docs/hardware-targets.md`](docs/hardware-targets.md).

---

## Planned

### M14 - audio input (#83-#85)

Output-only limits Tessera to synthesis. Add I2S capture (codec or ADC such
as a PCM1808), an input node type in the graph, and an end-to-end
input -> effect -> output path - the effects-pedal use case the CM4 form
factor is aimed at.

**Done when:** live audio in, through the reference low-pass plugin, out the
DAC, with measured round-trip latency published in
[`docs/latency.md`](docs/latency.md).

---

## Backlog (unscheduled)

- **Legacy IKOS pruning** - *done.* The pre-fork x86 IKOS teaching-OS tree
  (`kernel/`, `user/`, the x86 bootloaders, the network stack, GUI, terminal,
  USB, and their tests, headers, and docs) and the x86 half of the `Makefile`
  were removed, leaving an ARM-only Tessera tree; `make` now defaults to the
  ARM build. The history remains in git before the prune commit.
- **`raspi4b` CI job** - once CI runners have QEMU >= 9.0, run the existing
  test suite against the BCM2711 machine as a permanent gate (feeds M10).
- **SDK conformance tool** - ship the `make verify-plugin-abi` checks as a
  standalone tool third-party authors can run against their binaries before
  distributing them.

---

## Roadmaps and ideas

Two longer-form planning documents expand on the milestones above; neither is a
commitment, just a place to draw from:

- [`docs/hardware-targets.md`](docs/hardware-targets.md) - the boards and SoCs
  Tessera could target beyond the CM4, and the portability work (a board HAL,
  device-tree parsing) that makes new boards cheap.
- [`docs/feature-ideas.md`](docs/feature-ideas.md) - capability ideas
  (never-go-silent reliability, DSP building blocks, timing, routing, control,
  the plugin ecosystem) grouped by theme, with the isolation-differentiating
  ones flagged.


### M12 managed integration completion

The temporal runtime now manages admitted load/start/pause/rewire/unload,
bounded ABI/init/parameter/destructor calls and rollback on failures. EL0
return/current-process/budget state is per core, including nested-call FP
preservation. A four-core QEMU gate runs two independent DSP hosts with a
simultaneous trusted EL0 control client. See
[`docs/temporal-contracts.md`](docs/temporal-contracts.md) for the supported
frame-synchronous model and `docs/temporal-runtime-verification.md` for results.
Physical board acceptance remains M10; gapless topology replacement and
cross-core partitioning of one graph are not implied by this integration.
