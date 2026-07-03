# Feature ideas

A menu of capability ideas for Tessera, beyond the scheduled milestones in
[`MILESTONES.md`](../MILESTONES.md) and the board work in
[`docs/hardware-targets.md`](hardware-targets.md). This is a place to draw from,
not a commitment.

The lens: Tessera's differentiator is that plugins are **MMU-isolated,
CPU-budgeted, and syscall-gated**. Every serious embedded audio OS today runs
plugins in one shared address space (Elk's Sushi is a single Linux process), so
a faulty or hostile plugin can corrupt the host. The features worth the most are
the ones that turn Tessera's isolation into product value a single-process host
cannot safely offer. Those are marked **[isolation]** below.

## A. Never-go-silent reliability

The concrete payoff of the whole microkernel architecture. A pedal that dies on
stage is a failed product; isolation lets Tessera promise it will not.

- **Safe-mode bypass [isolation].** On catastrophic plugin failure, the platform
  auto-routes clean input -> output so audio never stops. The headline feature:
  it makes the isolation thesis audible, and it is demoable on QEMU today.
- **Glitch-free patch switching.** Crossfade between two graphs so swapping a
  patch mid-performance produces no click.
- **Plugin hot-reload [isolation].** Replace a plugin's ELF live with no
  dropout - a fast dev loop and a field-update mechanism, safe because the
  swap is contained to one address space.
- **Crash black-box [isolation].** Persist the last N audio blocks plus the
  faulting plugin's identity across reboot for post-mortem. Trivial with
  isolation; impossible when a crash takes down the whole process.
- **Per-plugin memory quota [isolation].** Complements the M12 CPU budget to
  complete the resource story: memory, time, and syscalls all bounded per
  plugin.

## B. DSP building blocks and a reference plugin suite

- **A DSP library in the SDK** - biquads, oscillators, delay lines, envelope
  followers, one-pole smoothers - so authors do not start from `sinf`.
- **A reference effects suite** that shows the platform off: tempo-synced delay,
  reverb, overdrive, compressor, 3-band EQ, chorus, noise gate, tuner.
- **IR convolution (cabinet sim)** - a deliberately heavy plugin that showcases
  the CPU-budget and isolation guarantees under real load.
- **A polyphonic synth voice plugin** - the ABI advertises synth voices; prove
  it end to end.

## C. Musical timing and transport

- **A master transport/clock** (tempo, bars/beats) shared with plugins via the
  ABI.
- **Tempo-synced parameters** (delay time, LFO rate locked to tempo) and **tap
  tempo**.
- **MIDI clock in/out sync**; an **arpeggiator / step-sequencer** node.

## D. Richer graph and routing

- **Explicit feedback edges** (a one-block delay) so feedback delay and reverb
  topologies are first-class - the graph is a strict DAG today.
- **Mixer / send-return bus nodes**, gain/pan nodes, and per-plugin **wet/dry +
  true bypass** (with hardware relay control for analog bypass).
- **Multi-channel buses** beyond stereo.

## E. Control and UX (the pedal front panel)

- **Control-surface mapping**: footswitches, rotary encoders, and an expression
  pedal mapped to parameters, with **MIDI-learn**.
- **On-device OLED UI**: patch/parameter browse and live CPU/headroom meters.
- **MIDI program change -> patch load**; patch **banks** on the SD card.
- **Desktop/remote editor** over USB serial or OSC - build patches from a
  laptop.

## F. Plugin ecosystem and ABI growth

- **ABI v1.1**: deliver MIDI/note events and transport into plugins, unlocking
  synths, arpeggiators, and MIDI effects - the natural next ABI bump.
- **Signed plugin packages [isolation].** A package format with signatures - the
  logical completion of the untrusted-plugin thesis: signed, sandboxed,
  quota'd, and revocable.
- **A Rust plugin SDK** - a thin wrapper over the C ABI to broaden authorship.
- **Embedded presets** in the plugin ELF; sample-rate / block-size negotiation.

## G. Developer experience

- **Offline plugin host** - run a plugin against a WAV file on the desktop for
  fast dev/test, reusing the existing host-test pattern.
- **Per-plugin profiler** surfaced in the shell and on the display.

## H. Signal quality and I/O

- **Denormal protection (FTZ/DAZ)** guaranteed on the audio path as a platform
  property.
- **96 kHz / variable sample rate** plus a sample-rate-conversion node.
- **Multi-in / multi-out** (stereo pairs, aux sends).
- **USB Audio Class** as an alternative transport (a laptop-interface mode).

## Highest-leverage pick

**Safe-mode bypass** (theme A). It is the one feature that turns the isolation
architecture into a tangible product guarantee - your pedal cannot go silent on
stage - it is demoable on QEMU today, and nothing built on a single-process host
can match it safely. It is the natural sequel to the M8 resilience demo and the
M12 budget enforcement: those *catch and kill* a bad plugin; safe-mode bypass is
what the audio path *does* in that moment.
