# Tessera milestones

The roadmap is organised as milestones, each a small set of GitHub issues with
a concrete **"done when"** criterion - a demo or measurement, not a feature
list. Milestones M0-M9 (issues #1-#40) and M12 (#77-#79) are complete in QEMU; this file records what
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

## M11 and M13 — integrated software acceptance

### M11 — multicore temporal scheduling (#74–#76)

Implemented in `temporal_multicore` and `audio_session`: CPU1–3 placement,
per-core admission with dependency/handoff timing, bounded current-frame
consumer waits, late-core attribution, and frame-boundary graph replacement.
The same actual ELF workload exceeds a frame in serial and completes on three
workers under the declared QEMU model. Cross-core PCM is compared with serial
PCM; spinning and memory-faulting plugins are contained on each worker.

### M13 — interactive control and session persistence (#80–#82)

The actual serial workstation uses the managed temporal engine for load,
unload, wire, parameters, contracts, placement, stats and FAT session save/load.
Acceptance drives the PL011, checks 18 live swaps without a deliberately paused
frame, and restores identical output in a second, fresh emulator process.
`make run-arm-workstation` provides a persistent QEMU frontend with `:reboot`.

Run `make test-arm-m11-m13`; see [`docs/m11-m13.md`](docs/m11-m13.md) and the
verification record for timing models, limitations and exact results. These
are software/emulator milestones. Physical CM4 I2S/SD and measured hardware
latency remain M10; no physical acceptance is inferred from these results.

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
