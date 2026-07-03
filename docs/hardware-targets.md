# Hardware targets: current and future

Tessera runs today on two things: the QEMU `virt` board (the always-on CI test
platform: MMU on, real exception vectors, EL0 plugins) and, as the in-progress
M10 milestone, the Broadcom **BCM2711** (Raspberry Pi 4 / CM4). This document
maps the realistic hardware Tessera could target beyond that, what each one
costs, and how each can be validated. It is a menu, not a commitment; the
milestone list in [`MILESTONES.md`](../MILESTONES.md) is where scheduled work
lives.

The thesis Tessera is proving - MMU-isolated audio plugins on ARM
application-class cores - is not tied to one SoC. Broadening the hardware is how
that generality gets demonstrated.

## What a new board actually costs (the porting surface)

Everything SoC-specific lives in a small, well-defined set of places. A new
board touches some subset of:

| Concern | Where it lives today | Varies by board? |
|---|---|---|
| CPU / ISA | `-mcpu=cortex-a72`, `smp.h` | A53 / A72 / A76, all AArch64 |
| RAM base + size | `arch/arm64/pmm.h` (`0x40000000`) | yes |
| Peripheral (MMIO) base | `pmm.h` / `mmu.c` (`0xFE000000` on BCM2711) | yes (per SoC) |
| Kernel load address | `arch/arm64/kernel.ld` (`0x80000` on Pi) | yes |
| Interrupt controller | `drivers/gic.c` (GIC-400 / GICv2) | **yes - the big one** |
| Generic timer | `arch/arm64/` timer (architected, `CNTFRQ`) | mostly portable |
| UART console | `drivers/uart_pl011.c` + mini-UART | yes (PL011 / 8250 / SoC) |
| SMP bring-up | PSCI (`smp.c`) | PSCI vs spin-table |
| SD / storage | EMMC2 (planned, M10) | yes |
| Audio path | `drivers/i2s.c` + `dma.c` (PCM/I2S + DMA + PCM5102) | **yes - per SoC + per codec** |

The interrupt controller and the audio path are the two hard parts; the rest is
base-address and constant work. The driver split Tessera already uses
(pure-C, host- and QEMU-testable logic separated from the SoC-only MMIO poke)
carries over directly to every target below.

## Enabling infrastructure (makes all boards cheap)

These are investments in *portability itself*. Doing them turns each new board
from a fork into a small plug-in.

1. **Board abstraction layer (HAL).** A board descriptor (bases, IRQ numbers,
   CPU count) plus a small vtable for the irqchip, timer, UART, GPIO, and
   I2S/DMA. Today those constants are compile-time literals; the HAL makes them
   a per-board struct.
2. **Interrupt-controller abstraction.** The current driver is GICv2 (GIC-400).
   A clean irqchip interface lets GICv3, the GIC-500, and the BCM legacy local
   interrupt controller (Pi 3) sit behind the same calls.
3. **Device Tree (FDT) parsing.** The standard mechanism for supporting many
   boards from one image: discover memory, peripherals, and interrupt mappings
   at boot instead of hard-coding them. Every board below ships a DTB.
4. **Multi-board build.** `make BOARD=pi4|pi3|virt|zcu102`, selecting the
   descriptor and load address, so one tree produces every image.

## Tier A - emulatable now (CI-validatable without a board)

These run on the QEMU we already have (8.2), so they can be brought up and
gated in CI immediately. **Caveat:** QEMU does not emulate the I2S/PCM audio
peripheral on any of these (same as `virt`), so they validate boot, the
interrupt controller, the timer, UART, SMP, and the MMU/plugin isolation - the
audio MMIO stays smoke-tested until real silicon.

| Target | SoC / CPU | Interrupt ctrl | Why it is worth doing | Effort |
|---|---|---|---|---|
| `raspi3b` | BCM2837 / 4x A53 | **BCM legacy (no GIC)** | A real second Broadcom board, cheap to buy; forces the irqchip + peripheral-base abstraction. Best first port. | M |
| `xlnx-zcu102` | ZynqMP / 4x A53 | **GIC-400** | Same interrupt controller as the BCM2711; the closest emulated rehearsal for the M10 CM4 bring-up. | M |
| `sbsa-ref` | SBSA / A57+ | **GICv3** | Standards-based Arm ServerReady platform; proves Tessera is not wired to one vendor and exercises a GICv3 driver. | M |
| `xlnx-versal-virt` | Versal / A72 | GICv3 | A72 (like the CM4) with GICv3; a second GICv3 datapoint. | M |

## Tier B - the Raspberry Pi family (reuses most of the CM4 work)

Broadcom boards share peripheral lineage, so once the CM4 (M10) lands, its
siblings are incremental.

| Board | SoC / CPU | Notes | Effort |
|---|---|---|---|
| Pi 4 / CM4 | BCM2711 / 4x A72 | The M10 target (in progress). GIC-400, periph base `0xFE000000`. | (M10) |
| Pi 3 / CM3 / Zero 2 W | BCM2837 / 2710, A53 | Emulatable via `raspi3b`; cheap boards. No GIC (legacy intc), periph base `0x3F000000`. | M |
| Pi 5 / CM5 | BCM2712 / 4x A76 | I/O moved behind the **RP1** southbridge over PCIe; peripheral access is substantially different. Higher effort. | L |
| Pi 2 | BCM2836 / A7 | 32-bit ARMv7 - **out of scope**, Tessera is AArch64-only. | - |

## Tier C - product-grade audio SoCs (datasheet-driven, need a board)

These are what a shippable pedal or synth would actually use. No convenient
emulation; each is real MMIO work against a datasheet and needs the physical
board to validate. Ordered by audio pedigree.

| SoC | CPU | Audio interface | Why | Effort |
|---|---|---|---|---|
| NXP i.MX8M / i.MX8M Plus | 4x A53 | **SAI** + strong audio subsystem | The audio-first pick: SAI, good docs, wide use in pro-audio gear; GIC-400 (familiar). | L |
| Rockchip RK3399 | 2x A72 + 4x A53 | I2S | Popular audiophile-SBC SoC (used in HiFi boards), well-documented; GIC-500. | L |
| TI Sitara AM62x | 4x A53 | McASP | Industrial, audio-capable, long-life supply; good for a fixed-function pedal. | L |
| Rockchip RK3588 | 4x A76 + 4x A55 | I2S / PDM | High-end, lots of DSP headroom; newer and more complex. | XL |
| Allwinner A64 / H6 | 4x A53 | I2S | Cheap, community-documented; lower audio pedigree. | L |
| Amlogic S905 | 4x A53 | I2S | Media-box heritage; abundant cheap boards. | L |

## Tier D - peripheral breadth (more hardware, orthogonal to the SoC)

"More hardware" is not only more SoCs; it is also more of the things hanging off
the audio bus. These reuse the whole CPU/SoC stack and only add a driver.

- **More DAC / codec support beyond the PCM5102:** WM8960, TLV320AIC3104,
  CS4272, ES9038. Most need a control-plane init over **I2C** (which itself is a
  small new driver), unlike the config-free PCM5102.
- **More capture ADCs beyond the PCM1808** (the M14 input codec).
- **USB Audio Class** (device or host): a completely different audio transport,
  opening laptop/interface use cases.
- **Control I/O:** I2C, SPI GPIO expanders, rotary encoders, and a small OLED
  for an on-device patch UI - the physical front panel of a pedal.

## Tier E - further afield (major lifts)

- **ARMv9 cores** (Cortex-A710 / A720): same AArch64 ISA family, so mostly a
  new board descriptor rather than an arch port.
- **RISC-V port:** QEMU ships a `virt` RISC-V machine. This is a genuine second
  architecture - a rewrite of `arch/` (entry, exceptions, page tables, syscall
  ABI) - but it would prove the microkernel design is not ARM-specific. A large,
  standalone effort.

## Suggested sequence

The cheapest path that maximizes generality and de-risks M10:

1. **HAL + irqchip abstraction** (Tier "enabling") - small, unblocks everything.
2. **`raspi3b` port** (Tier A) - validatable in CI now; exercises the
   abstraction against a no-GIC SoC and a second Broadcom base.
3. **`xlnx-zcu102`** (Tier A) - a GIC-400 target that rehearses the CM4 in
   emulation before the board arrives.
4. **Device tree parsing** - once two or three boards exist, replace the
   per-board constants with a DTB.
5. **CM4 / BCM2711** (M10) - the real-hardware milestone, now de-risked.
6. **One product-grade SoC** (Tier C, e.g. i.MX8M) - the first genuinely
   product-oriented platform.

Everything in Tier A is doable today with no new tools and lands as CI-gated
evidence; Tiers C and beyond wait on hardware.
