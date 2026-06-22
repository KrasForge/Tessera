# Tessera

**A microkernel audio operating system for ARM Cortex-A, forked from [IKOS](https://github.com/Ikey168/ikos).**

Tessera runs untrusted, third-party audio plugins — synth voices, effects,
sequencers — as **fully MMU-isolated processes** on ARM application-class
hardware. Each plugin lives in its own virtual address space. One plugin
crashing, hanging, or misbehaving cannot corrupt the host, other plugins, or
the audio engine. Ever.

---

## The gap this fills

Every serious embedded audio OS today is Linux underneath.
[Elk Audio OS](https://www.elk.audio/) — the closest prior art — runs plugins
inside **Sushi**, a single Linux process. Elk's own documentation states that
its real-time kernel runs in *"the same memory space as the traditional Linux
kernel."* Plugin isolation is not possible by design: a faulty or malicious
plugin shares an address space with the host and can corrupt everything in it.

On Cortex-M hardware the situation is worse — the MPU provides region
protection but no virtual address spaces, so real per-plugin isolation is
physically impossible regardless of what OS you build.

**On Cortex-A, the hardware can do it. Nobody has built it.**

The MMU is there. Virtual address spaces are there. The capability to load an
untrusted plugin binary, give it a clean isolated address space, and
fault-contain it with no shared state — all of that exists in the silicon. It
just requires a microkernel instead of Linux, and nobody has shipped one for
audio.

Tessera is that microkernel.

---

## Why this matters

The moment plugins come from *someone else* — a community, a marketplace, a
collaborator — isolation stops being an academic nicety and becomes the entire
safety model. Without it, loading an untrusted plugin is equivalent to running
arbitrary code with full access to your audio engine and hardware.

Tessera's thesis: **a real microkernel is the only correct foundation for an
open audio plugin platform.** Everything else is a workaround.

---

## Target hardware

**Primary: Raspberry Pi CM4 / Pi 4**

- Quad-core ARM Cortex-A72, full ARMv8-A MMU
- Fanless in commercial module enclosures (CM4 form factor)
- I2S audio via GPIO, proven in commercial audio hardware
- Broad bare-metal documentation and community

The CM4 is the SoC used in real commercial Eurorack modules and audio
appliances. It runs fanless, fits the form factors that matter (pedals,
modular), and gives the Cortex-A72 MMU that makes the whole isolation story
possible.

---

## Architecture

Tessera is a **microkernel**. The privileged kernel is minimal by design:

1. **Scheduler** — real-time, with a guaranteed audio-callback cadence
2. **VMM** — per-plugin virtual address spaces, MMU-enforced
3. **IPC** — message passing for control; shared-memory ring buffers for audio

Everything else runs unprivileged:

- **Plugin host** — loads, wires, and manages the audio graph
- **Plugins** — synths, effects, sequencers; each in its own address space
- **Drivers** — I2S, MIDI, CV; isolated from the kernel and from each other

Audio moves between plugins via **shared-memory ring buffers** — zero kernel
involvement per block, in the spirit of JACK. The kernel sets up the shared
region once; plugins read and write directly. No per-block syscalls, no copies.

---

## Relationship to IKOS

Tessera is a fork of [IKOS](https://github.com/Ikey168/ikos), an x86_64
microkernel. The architecture carries over directly:

- Virtual memory manager (`vmm.c`) — concept intact, page table format changes from x86_64 to ARMv8 translation tables
- IPC and syscall layer — ports cleanly
- Process model and scheduler — ports cleanly
- Audio subsystem — redesigned for I2S on ARM; AC97/x86 dropped

What gets replaced is the hardware layer: x86 boot → ARM boot, IDT/GDT →
ARM exception vectors, APIC → GIC, PCI → device tree / MMIO. The microkernel
design — the years of work — is unchanged.

---

## What Tessera is not

- Not a general-purpose OS — no desktop GUI, no browser, no server stack
- Not competing with Elk on latency — that problem is solved; this one isn't
- Not a Linux distribution — no co-kernel patch, no shared address space compromise
- Not chasing a hardware gap — the gap is architectural, not silicon

---

## Status

**Pre-alpha — ARM port in progress.** x86-specific boot and hardware layers are
being replaced. The microkernel core (VMM, IPC, scheduler) is inherited from
IKOS and is being adapted for ARMv8.

See [`MILESTONES.md`](./MILESTONES.md) for the roadmap.

---

## License

MIT — chosen to encourage a third-party plugin ecosystem.
