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

## Per-plugin memory quota

The M12 CPU budget bounds the *time* a plugin may take; this bounds the *memory*
it may hold, completing the resource-isolation story - memory, time, and
syscalls all bounded per plugin.

A plugin's footprint is fixed at load: its ELF PT_LOAD segments (including
`.bss`) plus the fixed stack, trampoline, param, and parameter-queue pages.
Plugins cannot allocate at runtime - there is no such syscall. So the quota is
enforced at load time on the image's **declared** page count, computed from the
program headers (`elf64_load_pages`), **before a single frame is committed**: a
plugin whose footprint exceeds its budget is refused with `PM_EQUOTA`, so a
greedy or hostile image cannot exhaust physical memory and starve the audio
engine or the other plugins. The budget is set with `pm_set_quota(m, pages)`
(0 = unlimited, the default, so existing behaviour is unchanged).

### How it is reproduced

```
# Pure charge/limit accounting (host, deterministic):
make test-arm-mem-quota

# End to end on QEMU virt:
make test-arm-mem-quota-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

The QEMU harness (`tests/arm64/virt/mem_quota_main.c`) sets a 32-page budget and
loads two images: the small reference low-pass (3 PT_LOAD pages) and a greedy
plugin with a 256 KiB `.bss` (65 PT_LOAD pages):

```
declared PT_LOAD pages: effect=3 greedy=65  quota=32 pages/plugin
effect: load=1 runs=1
greedy: rejected=1 (rc=-7) no-alloc-on-reject=1 loads-when-unlimited=1
leak: baseline=32158 after=32158 no-leak=1
MEM-QUOTA: PASS
```

- the small effect loads under budget and its `process_block` runs;
- the greedy plugin is refused (`rc=-7` = `PM_EQUOTA`), and **not one physical
  page was committed** for it (the free count is unchanged across the rejected
  load);
- with the budget lifted, the same image loads fine (a positive control: it was
  the quota, not a broken plugin);
- unloading everything returns the frame allocator exactly to baseline.

## Glitch-free patch switching

Swapping a patch mid-performance must not click. An abrupt cut from one graph's
DAC-bound output to another's leaves a step discontinuity in the waveform - an
audible click, the pedal equivalent of a scratchy switch. The crossfade
(`arch/arm64/xfade.c`) runs both the outgoing patch A and the incoming patch B
for a short window and mixes their blocks with a gain that ramps A down and B
up, so the waveform moves *continuously* from A to B.

### Mechanism

The ramp is a raised-cosine (Hann) curve in Q15 fixed point:

- its **slope is zero at both ends**, so the fade meets the steady signal on
  either side with no kink;
- the two gains **sum to exactly one** (`gA + gB == 32768`) at every step, so a
  constant signal - or two identical patches - passes through the mix
  bit-for-bit unchanged;
- the mix is `dst = (A*gA + B*gB) >> 15` on the int16 PCM blocks: pure
  fixed-point integer arithmetic, so it runs on the `-mgeneral-regs-only` audio
  path with no floating point, like the rest of the kernel signal handling.

The fade spans `XF_STEPS + 1` blocks; the first block is exactly A, the last
exactly B, and B then becomes the running patch so the caller can retire A.

### How it is reproduced

```
# Pure fixed-point mixer (host, deterministic):
make test-arm-patch-switch

# End to end on QEMU virt:
make test-arm-patch-switch-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

The QEMU harness (`tests/arm64/virt/xfade_main.c`) plays steady patch A
(level 12000), crossfades to patch B (level 4000), and plays steady B, feeding
every DAC-bound block through the mixer:

```
blocks=23 fade=17  pre-A=3/3 post-B=3/3 switches=1
never-silent=23/23  fade 12000->4000 monotonic=1  max-step=781 (abrupt-cut=8000)  gains-unit=1
PATCH-SWITCH: PASS
```

- **never-silent = 23/23** - every block carries sound, across the switch.
- **pre-A / post-B exact** - before the fade the DAC is exactly patch A, after it
  exactly patch B.
- **monotonic, max-step = 781** - through the fade the level moves monotonically
  A -> B, and the largest block-to-block step is **781**, an order of magnitude
  below the **8000** step an abrupt cut would make. The abrupt step is the
  click; the crossfade removes it.
- **gains-unit = 1** - the two gains sum to unity at every ramp step.

## Plugin hot-reload

Replacing a plugin's ELF live - a fast dev loop, and a field-update path - must
not leave a gap in the audio. This is safe precisely because plugins are
MMU-isolated: the new version (`arch/arm64/hot_reload.c` drives the sequence)
is loaded into its **own fresh address space** while the old version keeps
running in its own, so the two coexist with no shared state, and the swap is a
single pointer flip at a block boundary. A single-process host cannot do this
safely - swapping code under a running engine risks the whole process.

### Mechanism

The reload state machine guarantees:

- the running generation produces **every** block until the new one is fully
  loaded and initialised - no block is ever produced by a half-ready version
  (the no-dropout guarantee);
- the swap commits at exactly one block boundary, after which the old generation
  is retired and its address space freed;
- a load that **fails** leaves the running version in place, unchanged (no
  dropout, no regression);
- only one reload is in flight at a time and generations advance monotonically,
  so a retired version never runs again.

### How it is reproduced

```
# Pure reload sequencing (host, deterministic):
make test-arm-hot-reload

# End to end on QEMU virt:
make test-arm-hot-reload-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

The QEMU harness (`tests/arm64/virt/hot_reload_main.c`) drives the **real**
plugin manager: it loads the reference effect as generation 0, reloads it as
generation 1 into a second isolated address space while generation 0 is still
running, swaps production at a block boundary, and retires generation 0:

```
gen0-pid=1 gen1-pid=2 distinct=1 both-live=1 prepared=1
swap: to-new=1 retired-old=1 old-gone=1 new-live=1 swaps=1
never-silent=6/6  leak: baseline=... after=... no-leak=1
```

- **distinct / both-live** - the two generations are separate isolated
  processes (different pids) and both are live at the moment of overlap.
- **never-silent = 6/6** - every block carries sound, across the swap - no
  dropped block.
- **old-gone** - after the swap the retired pid no longer resolves; the old
  instance is genuinely gone.
- **no-leak** - unloading everything returns the frame allocator to baseline, so
  the retired address space was fully reclaimed.

## Planned in this theme

Further never-go-silent work (to be built to the same bar - mechanism, host
tests, a QEMU harness): a crash black-box that persists the last blocks and the
faulting plugin across a reboot.
