# Tessera Plugin ABI v1.0

**Status: stable (frozen).** This document is the complete, self-contained
specification for building an audio plugin that the Tessera host can load and
run. It freezes the interface introduced in issue #23; nothing in this document
changes without a **major** version bump (see [Versioning](#versioning)).

The normative source of the constants and symbol names is
[`include/plugin_abi.h`](../include/plugin_abi.h). A third party needs only that
one header (it depends on nothing but `<stdint.h>`) and this document to build a
plugin with a stock `aarch64` toolchain; no Tessera kernel sources are required.

- [1. Overview](#1-overview)
- [2. Required exports](#2-required-exports)
- [3. Calling convention](#3-calling-convention)
- [4. Binary format](#4-binary-format)
- [5. The sandbox: what a plugin may touch](#5-the-sandbox-what-a-plugin-may-touch)
- [6. Real-time constraints](#6-real-time-constraints)
- [7. Versioning](#7-versioning)
- [8. Loading and validation](#8-loading-and-validation)
- [9. A complete plugin, from scratch](#9-a-complete-plugin-from-scratch)
- [10. Conformance checklist](#10-conformance-checklist)

---

## 1. Overview

A Tessera plugin is a standalone AArch64 ELF binary that exports five C
functions. The host loads it into an isolated virtual address space, resolves
the five symbols by name, checks the ABI version, then drives the plugin from
the real-time audio engine. A plugin can reach only its own memory and the
audio buffers the host hands it; any attempt to reach outside faults and the
host kills the plugin without disturbing the rest of the system.

The interface is plain C: AAPCS64 calling convention, no name mangling, no C++
runtime, no libc, no dynamic linker.

---

## 2. Required exports

A plugin **must** export exactly these five symbols, with C linkage (no name
mangling) and these exact signatures:

```c
uint32_t plugin_abi_version(void);
int      plugin_init(uint32_t sample_rate, uint32_t block_size);
void     plugin_process_block(const float *in_l, const float *in_r,
                              float *out_l, float *out_r, uint32_t n_frames);
void     plugin_set_param(uint32_t param_id, float value);
void     plugin_destroy(void);
```

### `plugin_abi_version`
Returns the ABI version the plugin was built against, i.e.
`TESSERA_PLUGIN_ABI_VERSION` (see [Versioning](#versioning)). The host calls
this **first**, before `plugin_init`, and rejects the plugin if the major
version does not match. Must be safe to call before `plugin_init`; it must not
depend on any state that `plugin_init` establishes.

### `plugin_init`
One-time setup at load. `sample_rate` (Hz) and `block_size` (frames per
`plugin_process_block` call) are **fixed for the plugin's lifetime**. Returns
`TESSERA_PLUGIN_OK` (0) on success, or a negative `TESSERA_PLUGIN_E*` code to
refuse the load:

| Code | Value | Meaning |
| --- | --- | --- |
| `TESSERA_PLUGIN_OK`       | `0`  | Success. |
| `TESSERA_PLUGIN_EVERSION` | `-1` | Unsupported sample rate / block size. |
| `TESSERA_PLUGIN_ENOMEM`   | `-2` | Could not allocate plugin state. |
| `TESSERA_PLUGIN_EINVAL`   | `-3` | Invalid argument. |

`plugin_init` runs off the audio path, so it may do non-real-time work.

### `plugin_process_block`
Process one block of de-interleaved stereo audio. `in_l`/`in_r` are the left and
right **input** buffers and `out_l`/`out_r` the **output** buffers, each exactly
`n_frames` `float`s (`n_frames == block_size`). Samples are 32-bit IEEE-754
floats, nominal range `[-1.0, +1.0]`. The buffers do not overlap unless the
plugin is prepared to handle aliasing. This function runs on the real-time audio
thread and is bound by the [real-time constraints](#6-real-time-constraints).

### `plugin_set_param`
Update a parameter by id. Called from the audio path; must be real-time-safe and
wait-free (a single store or a lock-free publish). Unknown `param_id`s must be
ignored. Parameter ids and their meanings are defined by the plugin.

### `plugin_destroy`
Release anything acquired in `plugin_init`. Called once before unload, off the
audio path. After it returns, no other entry point will be called.

The canonical symbol-name macros are in the header
(`TESSERA_PLUGIN_SYM_*`).

---

## 3. Calling convention

- **AAPCS64** (the standard AArch64 procedure call standard). Integer and
  pointer arguments in `x0`–`x7`; return value in `x0`.
- **Floating-point** arguments and returns use the NEON/FP registers (`v0`–`v7`,
  scalar `s`/`d`). `plugin_set_param`'s `float value` therefore arrives in `s0`.
  Audio sample buffers are passed by pointer (`in_l` etc. in `x0`–`x3`,
  `n_frames` in `x4`).
- The plugin runs at **EL0** (unprivileged). It may freely use the general and
  NEON/FP register files; the host preserves and restores its own state across
  the call.
- There is **no thread-local storage**, no global constructors/destructors, and
  no C++ runtime. Global and static variables in the plugin's own `.data`/`.bss`
  are fine and persist across calls.

---

## 4. Binary format

A plugin is delivered as a single ELF file meeting all of:

- **Class / encoding:** ELF64, little-endian (`ELFCLASS64`, `ELFDATA2LSB`).
- **Machine:** AArch64 (`EM_AARCH64`, 183).
- **Type:** `ET_EXEC` (a static executable linked at the Tessera user base) or
  `ET_DYN`. The reference plugins are `ET_EXEC`.
- **Position-independent code:** compile with `-fPIC`.
- **Freestanding:** compile `-ffreestanding`; the plugin must not reference the
  C runtime or libc. It links against no external libraries.
- **No dynamic linker:** no `PT_INTERP`, no runtime symbol resolution. The
  binary is self-contained: it must have **zero undefined (imported) named
  symbols**. Importing any symbol outside the plugin ABI (libc, kernel, another
  plugin) causes the host to reject the binary at load.
- **Sections:** standard `.text`, `.rodata`, `.data`, `.bss`, `.symtab`,
  `.strtab`. Each loadable section is page-aligned so the host can map it with
  the correct permissions (RX for code, R for rodata, RW for data/bss). Build
  metadata sections (`.comment`, `.note*`, `.eh_frame*`, `.ARM.attributes`) are
  discarded by the reference link script.
- **Symbol table:** the five ABI symbols must be present as defined, globally
  visible symbols in `.symtab` (the host resolves by name).

The reference toolchain flags and link script are:

```make
PLUGIN_CFLAGS = -mcpu=cortex-a72 -ffreestanding -fPIC -O2 -std=c11 -Wall -Wextra
```

The provided link script [`plugins/plugin.ld`](../plugins/plugin.ld) places the
plugin at the Tessera user base (`0x8000000000`), page-aligns each output
section, sets `ENTRY(plugin_init)`, and discards build metadata. Use it verbatim
unless you understand the address-space contract in
[§5](#5-the-sandbox-what-a-plugin-may-touch).

---

## 5. The sandbox: what a plugin may touch

Each plugin runs in its own virtual address space. The **only** memory it can
reach is:

- its own `code` (RX), `rodata` (R), `data`/`bss` (RW), and stack (RW);
- the audio buffers the host passes into `plugin_process_block`
  (and any host-provided shared buffer mapped at a fixed address per the host's
  graph contract);
- a small per-plugin parameter region the host manages.

Everything else is unmapped. Specifically, a plugin can **not** see kernel
code/data, hardware MMIO, or any other plugin's address space. Consequences:

- A wild or null pointer access takes a data abort; the host kills the plugin.
- A plugin may **not** issue a syscall (`SVC`) of its own. The host permits an
  `SVC` only from its controlled entry trampoline; an `SVC` from the plugin body
  (including from `plugin_process_block`) is a protocol violation and the host
  kills the plugin. In practice this means: do not attempt any system call.

Killing a plugin frees all of its resources and never disturbs the audio engine
or other plugins (see the resilience demo in [`docs/demo.md`](demo.md)).

---

## 6. Real-time constraints

`plugin_process_block` and `plugin_set_param` run inside the real-time audio
callback on the dedicated audio core. They **must** be real-time-safe:

- **No memory allocation.** No `malloc`/`free`/`new`/`delete`. Pre-allocate all
  state in `plugin_init`.
- **No system calls** of any kind (and none are reachable anyway; see
  [§5](#5-the-sandbox-what-a-plugin-may-touch)).
- **No blocking.** No locks another thread can hold, no waiting on I/O, no
  spinning on external state.
- **No unbounded work.** Work must be `O(n_frames)` and finish well within one
  block period (`block_size / sample_rate` seconds).
- **No non-reentrant library functions.** The plugin is freestanding; there is
  no libc to call. Do not rely on hidden global state (e.g. `errno`, locale, a
  shared PRNG) that is not your own.

`plugin_init` and `plugin_destroy` are the only entry points that may do
non-real-time work (allocation, table precomputation); they never run from the
audio callback.

### Host enforcement (M12, issue #78)

The constraints above are not merely advisory: the host **enforces the time
budget**. This is host policy layered on top of the frozen v1.0 ABI - nothing
here changes the ABI, the exports, or their signatures.

- Every plugin gets a **per-block CPU budget**: by default a fair share of the
  block period across the plugins scheduled on its core, or a value set
  explicitly through the control plane (`gc_set_budget`).
- A plugin still inside `plugin_process_block` at its budget boundary is
  **preempted mid-block** by the kernel's budget timer - it does not get to
  finish the block, and the host emits **silence** downstream for that block
  (an *offence*, visible as `offences=` in the `plugin_time:` stats line).
- Offences escalate: a plugin that offends on **3 consecutive blocks is
  killed** and unloaded, with a `[budget]` log line naming it and the measured
  time. A clean block resets the streak - a plugin that recovers after a
  transient overrun is forgiven (but its offence count remains visible).
- A preempted plugin's block-local state may be inconsistent when it is next
  entered (its stack is reset, statics persist). A plugin that cannot
  tolerate this was already violating the no-unbounded-work rule.

For plugin authors the message is unchanged: finish well within the block
period. The enforcement exists so that a plugin which does not - by bug or by
malice - costs one block of its own silence, not the graph's.

---

## 7. Versioning

The ABI version is a 32-bit value: **major** in the high 16 bits, **minor** in
the low 16 bits.

```c
#define TESSERA_PLUGIN_ABI_VERSION_MAJOR 1u
#define TESSERA_PLUGIN_ABI_VERSION_MINOR 0u
#define TESSERA_PLUGIN_ABI_VERSION \
    ((TESSERA_PLUGIN_ABI_VERSION_MAJOR << 16) | TESSERA_PLUGIN_ABI_VERSION_MINOR)
```

`plugin_abi_version()` must return `TESSERA_PLUGIN_ABI_VERSION` (for v1.0, that
is `0x00010000`).

**Compatibility rule.** The host accepts a plugin only when:

1. the plugin's **major** version equals the host's major version, **and**
2. the plugin's **minor** version is `<=` the host's minor version.

This makes minor revisions additive and backward-compatible: a plugin built
against v1.0 keeps loading on a v1.x host, while a host will refuse a plugin
built against a newer major line it does not understand, before running any of
its code.

- **Minor bump (1.0 -> 1.1):** additive, backward-compatible changes only (e.g.
  new optional host-provided regions, new `TESSERA_PLUGIN_E*` codes). Existing
  v1.0 plugins continue to load and run unchanged.
- **Major bump (1.x -> 2.0):** any breaking change (signature change, new
  required export, changed semantics). Requires recompilation against the new
  header.

v1.0 is frozen: the header carries a compile-time assertion that the major
version is 1, so an accidental break is caught at build time.

---

## 8. Loading and validation

When the host loads a plugin it, in order:

1. validates the ELF (class, encoding, machine, type, in-bounds headers);
2. rejects any binary with disallowed imports (undefined named symbols);
3. maps each `PT_LOAD` segment into a fresh isolated address space with the
   segment's permissions;
4. resolves the five ABI symbols by name (`plugin_init` is mandatory);
5. calls `plugin_abi_version()` at EL0 and rejects a mismatched **major**
   version before running anything else;
6. calls `plugin_init(sample_rate, block_size)`; a non-zero return refuses the
   load.

Thereafter the host calls `plugin_process_block` once per audio block,
`plugin_set_param` when a parameter changes, and `plugin_destroy` once at
unload. Any fault or illegal `SVC` during these calls terminates only that
plugin.

---

## 9. A complete plugin, from scratch

A minimal gain plugin. This is the entire source; it needs only `plugin_abi.h`.

```c
/* mygain.c - multiply both channels by a gain parameter. */
#include "plugin_abi.h"

static float g_gain = 1.0f;   /* param id 0 */

uint32_t plugin_abi_version(void)
{
    return TESSERA_PLUGIN_ABI_VERSION;
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate; (void)block_size;
    g_gain = 1.0f;
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    for (uint32_t i = 0; i < n_frames; i++) {
        out_l[i] = in_l[i] * g_gain;
        out_r[i] = in_r[i] * g_gain;
    }
}

void plugin_set_param(uint32_t param_id, float value)
{
    if (param_id == 0)
        g_gain = value;          /* wait-free: a single store */
}

void plugin_destroy(void) { }
```

Build it (copy `include/plugin_abi.h` and `plugins/plugin.ld` next to your
source, or point `-I`/`-T` at them):

```sh
aarch64-linux-gnu-gcc -mcpu=cortex-a72 -ffreestanding -fPIC -O2 -std=c11 \
    -I. -c mygain.c -o mygain.o
aarch64-linux-gnu-ld -T plugin.ld -o mygain.elf mygain.o
```

Verify it is self-contained (no undefined named imports) and exports the ABI:

```sh
aarch64-linux-gnu-readelf -h mygain.elf | grep AArch64
aarch64-linux-gnu-readelf -sW mygain.elf | grep -E 'plugin_(abi_version|init|process_block|set_param|destroy)'
aarch64-linux-gnu-readelf -sW mygain.elf | awk '$7=="UND" && $8!=""'   # must be empty
```

Copy `mygain.elf` to the SD card (or ramdisk) and load it by path, e.g.
`sys_plugin_load("/sd/mygain.elf")`.

---

## 10. Conformance checklist

A conforming v1.0 plugin:

- [ ] exports `plugin_abi_version`, `plugin_init`, `plugin_process_block`,
      `plugin_set_param`, `plugin_destroy` with the exact signatures in
      [§2](#2-required-exports);
- [ ] returns `TESSERA_PLUGIN_ABI_VERSION` from `plugin_abi_version()`;
- [ ] is a little-endian AArch64 ELF (`ET_EXEC` or `ET_DYN`), built `-fPIC
      -ffreestanding`;
- [ ] has **no** undefined (imported) named symbols;
- [ ] treats `sample_rate` and `block_size` as fixed after `plugin_init`;
- [ ] does no allocation, syscalls, blocking, or unbounded work in
      `plugin_process_block` / `plugin_set_param`;
- [ ] touches only its own memory and the host-provided audio buffers.

The reference plugins in `plugins/` are verified against this checklist by
`make verify-plugin-abi` (run in CI).
