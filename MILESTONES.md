# Tessera milestones

The roadmap is organised as milestones, each a small set of GitHub issues with
a concrete **"done when"** criterion - a demo or measurement, not a feature
list. Milestones M0-M9 (issues #1-#40) are complete; this file records what
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

---

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

### M11 - multi-core plugin scheduling (#74-#76)

The audio core owns CPU0; today the rest of the graph shares it. CPU1-3 are
idle capacity. Schedule graph nodes across the secondary cores with
topology-aware ordering (a node runs only after its upstream edges have
produced), keeping CPU0's cadence untouched.

**Done when:** a graph that demonstrably overruns on a single core runs
without overruns when spread across CPU1-3, and killing a plugin on any core
disturbs neither the audio cadence nor plugins on other cores.

### M12 - per-plugin CPU budget enforcement (#77-#79)

The sandbox contains memory (MMU) and syscalls (SVC gate), but a plugin that
spins forever in `process_block` still starves the graph - the watchdog only
observes the global overrun. Account time per plugin per block, give each
plugin a budget, and neutralize (mute, then kill) a plugin that repeatedly
exceeds it. This extends isolation from memory safety to time safety and
completes the untrusted-plugin story.

**Done when:** a `hog` test plugin (infinite loop in `process_block`) joins
`crash` and `evil` in the resilience demo and is detected and killed within a
bounded number of blocks while the good plugin never misses one.

### M13 - interactive control shell (#80-#82)

Today the control plane is C code calling syscalls (#30) and patch files
(#40). Provide a serial shell over UART: `load` / `unload` / `wire` /
`unwire` / `set-param`, `patch save` / `patch load`, plus `ls` for the graph
and `stats` for the latency counters.

**Done when:** a user with nothing but a serial terminal can build a patch
from plugins on the SD card, tweak parameters live, save it, reboot, and
reload it - without compiling any C.

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

- **Legacy IKOS pruning** - `docs/implementation/` and `tests/` still carry
  x86-era IKOS material (GUI, network stack, terminal, USB) that predates the
  fork and misrepresents the project to new readers; prune it or move it under
  a clearly-labelled legacy directory.
- **`raspi4b` CI job** - once CI runners have QEMU >= 9.0, run the existing
  test suite against the BCM2711 machine as a permanent gate (feeds M10).
- **SDK conformance tool** - ship the `make verify-plugin-abi` checks as a
  standalone tool third-party authors can run against their binaries before
  distributing them.
