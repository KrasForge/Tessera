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

## Crash black-box

When a plugin faults, isolation lets Tessera catch and kill it while the audio
keeps running (M8/M12) - but the evidence of *why* it died is gone by the next
reboot. The black box (`arch/arm64/blackbox.c`) is a flight recorder: it keeps
the last N DAC-bound blocks in a small circular buffer, and when a plugin is
killed it freezes a snapshot - those blocks plus the faulting plugin's identity
and the fault cause - and serialises it to a reserved store that survives a
reboot. After the reboot the snapshot is parsed back for post-mortem: which
plugin, why, and exactly what the audio was doing in the blocks leading up to
the fault.

This is trivial **because** of isolation - the fault is contained to one address
space, so the recorder and the store are intact after the kill. On a
single-process host a crash takes down the whole engine, recorder and all, and
there is nothing left to persist.

### Mechanism

- the recorder keeps the last `BB_BLOCKS` blocks in a circular buffer, copied as
  raw 32-bit words (FP-free);
- on a kill, `bb_capture` freezes the faulting plugin's pid, name, and cause
  (MMU abort / forbidden syscall / budget overrun) and latches - the first
  crash wins;
- the snapshot serialises little-endian with an FNV-1a checksum, so a corrupt
  store is rejected on parse rather than yielding a bogus post-mortem;
- after a reboot, `bb_parse` recovers the identity and the pre-crash blocks
  bit-for-bit.

### How it is reproduced

```
# Pure recorder + serialise/parse (host, deterministic):
make test-arm-blackbox

# End to end on QEMU virt:
make test-arm-blackbox-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

The QEMU harness (`tests/arm64/virt/blackbox_main.c`) records the reference
effect's blocks, injects a genuine EL0 fault (the M8 crash plugin, MMU-caught,
isolated run returns -1), captures the snapshot, serialises it, "reboots" into a
fresh recorder, and recovers the post-mortem:

```
recorded=6/6 never-silent=6/6 fault-caught=1 serialized=... bytes
recovered: pid=1 cause=1 block=6 name0=e count=4
checks: identity=1 blocks-bit-exact=1 corrupt-rejected=1
```

- **fault-caught** - the fault was really contained (the isolated run returned
  -1), not simulated.
- **identity** - after the reboot the recovered snapshot names the faulting
  plugin (pid, name) and the cause (MMU), at the right block index.
- **blocks-bit-exact** - the last N pre-crash blocks survived the
  serialise/reboot/parse cycle bit-for-bit.
- **corrupt-rejected** - a single flipped byte in the store is rejected by the
  checksum, so a post-mortem is never built from corrupt data.

## Theme A complete

The never-go-silent theme now spans the full resource and reliability story:
safe-mode bypass (the audio path heals to dry-through when an effect dies),
per-plugin memory quota (memory bounded like the M12 CPU budget and the SVC
gate), glitch-free crossfaded patch switching, plugin hot-reload without a
dropout, and this crash black-box. Each is contained to one address space -
exactly what a single-process audio host cannot safely offer.

## Signed plugin packages (Theme F, issue #125)

Isolation, memory/CPU quotas, and the syscall gate contain a *loaded* plugin;
signed packages decide *whether to load it at all*. A plugin is distributed as a
package - a fixed header (magic, format/ABI versions, a signing-key id, capability
flags, and the plugin name), the ELF payload, and a trailing 32-byte MAC - and it
is authenticated on-device before it is ever mapped (`arch/arm64/package.c`).

- **Authenticity + integrity** come from an HMAC-SHA256 (`arch/arm64/sha256.c`,
  verified against the NIST and RFC 4231 vectors) over the header and payload
  under a provisioning key. The MAC is compared in constant time, so a wrong or
  tampered package fails without leaking where it first differed. HMAC is
  symmetric; a production build would swap in an asymmetric signature, but the
  format, the bounds-checked parse, the revocation check, and the constant-time
  compare are unchanged by the primitive.
- **Revocation.** Each package names its signing-key id. A compromised signer's id
  is added to a revocation list and its packages are rejected - after
  authentication, so the id is trustworthy - without a firmware change.
- **Untrusted input.** The verifier is fed an arbitrary blob, so every length is
  checked against the buffer first: a blob too small for a header + MAC, a bad
  magic or format, or an over-long declared payload length are all rejected before
  any payload byte is read.

The whole path is integer-only and links into the `-mgeneral-regs-only` kernel, so
verification runs on-device. Covered by `make test-arm-sha256` and
`make test-arm-package`.

## Parser fuzzing (Theme M16, issue #169)

Tessera parses several byte streams from untrusted sources - OSC editor messages
(#123), USB Audio descriptors (#133), signed packages (#125), the VideoCore
mailbox (#105), and embedded presets (#127). Each is individually bounds-checked;
`tests/arm64/fuzz_parsers.c` proves it continuously by flooding every one of them
with random and mutated bytes under AddressSanitizer + UBSan, so an out-of-bounds
read, an integer overflow, or a non-terminating loop trips the sanitizer and fails
the build.

It is a self-contained, **deterministic** fuzzer - a seeded LCG and a fixed
iteration budget - so it needs no libFuzzer/clang and runs the same everywhere,
making it a CI gate rather than an occasional manual run. Each parser is fed both
fully random buffers and mutated copies of a valid seed message; adding a new
parser is one entry in the target table. Covered by `make test-arm-fuzz` (60k
rounds per parser).

## Chaos-mode resilience gate (Theme M16, issue #170)

The M8 resilience demo kills one malicious plugin and shows audio survives.
`tests/arm64/chaos_test.c` generalises that into a continuous gate: over a long,
**seeded** soak it injects faults - MMU abort, budget overrun, syscall abuse, and
outright kill - into a multi-effect chain and, every block, asserts the platform's
safe-mode bypass contains each one. A dead node passes its dry input through, so
the DAC never sees a silent gap.

Every round the harness drives a non-silent source block through the chain, kills
and reloads nodes on a random schedule, and checks the invariants: each faulted
node is bypassed and emits its dry input (identified and contained), and the
DAC-bound block is never all-zero. A 20 000-round soak injects hundreds of faults
and reloads with **zero dropouts**. Because it is deterministic it runs as a CI
gate, not a one-off demo. Covered by `make test-arm-chaos`.

## Secure and measured boot (Theme M21, issue #193)

Signed plugin packages (issue #125) authenticate a plugin before it is mapped;
secure boot extends the same trust down to the boot chain so the *kernel image
itself* is authenticated before it runs (`arch/arm64/secureboot.c`). A boot image
is a fixed header (magic, versions, load address, payload length, and the
payload's SHA-256) followed by the kernel payload and a trailing HMAC-SHA256 over
header+payload under a provisioning boot key.

- **Verified boot** - `secureboot_verify` recomputes the payload hash and checks
  it against the header (integrity), then authenticates header+payload with the
  key in constant time (authenticity). A swapped or corrupted image is rejected
  before the jump to the kernel: a flipped payload byte fails the hash, a flipped
  header byte or the wrong key fails the MAC. The header is untrusted, so every
  length is bounds-checked (a too-short blob, a bad magic, or an over-long payload
  length are rejected before any payload byte is read).
- **Measured boot** - a PCR-style measurement register folds each boot stage into
  a running hash chain, `m = SHA-256(m || SHA-256(stage))`. The chain is
  deterministic and order-sensitive, so the final value attests exactly what
  booted - even a stage loaded without verification is still measured, and any
  change to a stage or its order changes the measurement.

Reuses `arch/arm64/sha256.c`; integer-only and libc-free, so it links into the
first-stage / kernel. The on-board first-stage loader that runs this is hardware;
the verify and measure logic is validated by `make test-arm-secureboot`.
