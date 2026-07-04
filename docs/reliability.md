# Reliability: never go silent (Theme A)

Tessera's differentiator is that plugins are MMU-isolated, CPU-budgeted, and
syscall-gated: a faulty or hostile plugin is caught and killed while the audio
engine keeps running (the M8 resilience demo, the M12 budget kill). This theme
turns that containment into a product guarantee - the audio path never goes
silent - which a single-process audio host (Elk/Sushi on Linux, where a plugin
fault takes down the whole engine) cannot safely offer.

## Safe-mode bypass

When an effect in the signal path suffers a fatal fault - an EL0 MMU data abort,
a forbidden syscall, or a budget kill - the node stops producing output. Its
consumer downstream (ultimately the DAC) would then be fed nothing and go
silent: a pedal dead on stage. Safe-mode bypass detects the dead node and routes
its **input** straight through to its consumer, so the path heals to
clean-through (the dry signal) and audio never stops.

### Mechanism

The decision is one pure, FP-free step per block (`arch/arm64/safe_bypass.c`),
so it runs on the `-mgeneral-regs-only` audio path and is exhaustively
unit-tested:

- while the effect is alive, its output is forwarded to the consumer;
- once it has died, the effect's input is forwarded instead (a dry passthrough),
  and the bypass **latches** - a killed plugin cannot silently come back;
- if the dead node had no upstream (a source), the consumer is fed silence
  rather than stale garbage.

The signal samples are copied as raw 32-bit words, so no floating point is
touched on the audio path.

### How it is reproduced

```
# Pure resolve logic (host, deterministic):
make test-arm-safe-bypass

# End to end on QEMU virt:
make test-arm-safe-bypass-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

The QEMU harness (`tests/arm64/virt/safe_bypass_main.c`) builds a real signal
path input -> effect -> DAC through the M6 reference low-pass. Partway through,
the effect suffers a genuine EL0 fault (the M8 crash plugin, null-dereferencing
in `process_block`; the MMU traps it and the isolated run returns -1). The
harness asserts the product-relevant facts:

```
blocks=8 trigger=4  normal=4 bypass=4
never-silent=8/8  from-live-effect=4/4  filter-altered=4/4  dry-passthrough=4/4  fault-caught=1
SAFE-BYPASS: PASS
```

- **never-silent = 8/8** - every DAC block carries sound, across the fault.
- **from-live-effect = 4/4** and **filter-altered = 4/4** - before the fault the
  DAC carried the effect's output, which the filter had audibly altered away
  from the dry input, so the signal genuinely came from the live effect.
- **fault-caught = 1** - the effect's fault was really contained (the isolated
  run returned -1), not merely simulated.
- **dry-passthrough = 4/4** - after the fault, the DAC carried the dry input,
  bit-exactly, for every remaining block.

Safe-mode bypass is the audible sequel to the M8 resilience demo and the M12
budget kill: those *catch and kill* a bad plugin; safe-mode bypass is what the
audio path *does* in that moment so it never falls silent.

## Planned in this theme

Further never-go-silent work (each to be built to the same bar - mechanism, host
tests, a QEMU harness): glitch-free crossfaded patch switching, plugin
hot-reload without a dropout, a crash black-box that persists the last blocks
and the faulting plugin across a reboot, and per-plugin memory quotas
(completing the resource-isolation story alongside the M12 CPU budget).
