# Changelog

All notable changes to Tessera's public interfaces are recorded here. This file
tracks the **plugin ABI** in particular, which third parties depend on.

The format is loosely based on [Keep a Changelog](https://keepachangelog.com/),
and the plugin ABI follows the versioning policy in
[`docs/plugin-abi.md`](docs/plugin-abi.md).

## Plugin ABI

### v1.1.0 - note events and transport (issue #124)

Backward-compatible (minor) addition. A v1.0 plugin loads and runs unchanged on
a v1.1 host; a v1.1 plugin is refused by an older host (the host applies
`tessera_abi_compatible()`: major must match, minor must be `<=` the host's).
`TESSERA_PLUGIN_ABI_VERSION == 0x00010001`.

- **New optional surface:** the host maps a second lock-free SPSC queue into the
  plugin at `TESSERA_EVENT_QUEUE_VA`, carrying MIDI-shaped note/CC events
  (`tessera_note_event_t`) and a per-block transport snapshot
  (`tessera_transport_t`: tempo, bar/beat/tick, ppq). Drained with
  `tessera_event_read()` / `tessera_transport_read()` from `libtessera.a`.
- **No change to the five required exports** or their signatures; the frozen v1
  contract is untouched. A v1.0 plugin simply never reads the queue.
- Unlocks synth voices, arpeggiators, and MIDI effects. Layout in
  [`sdk/tessera.h`](sdk/tessera.h); spec in
  [`docs/plugin-abi.md`](docs/plugin-abi.md) §11.

### v1.0.0 - frozen (issue #37)

First stable release of the Tessera Plugin ABI. Frozen: no change without a
major version bump.

- **Required exports** (C linkage, exact signatures): `plugin_abi_version`,
  `plugin_init`, `plugin_process_block`, `plugin_set_param`, `plugin_destroy`.
- **Calling convention:** AAPCS64; float arguments in NEON registers; plugin
  runs at EL0.
- **Binary format:** little-endian AArch64 ELF (`ET_EXEC`/`ET_DYN`), built
  `-fPIC -ffreestanding`, self-contained (no undefined imports, no libc, no
  dynamic linker), page-aligned loadable sections.
- **Sandbox:** a plugin may touch only its own memory and the host-provided
  audio buffers; wild/null accesses and any `SVC` from the plugin body are fatal
  to that plugin only.
- **Real-time contract:** no allocation, syscalls, blocking, or unbounded work
  in `plugin_process_block` / `plugin_set_param`.
- **Versioning:** 32-bit `MAJOR<<16 | MINOR`; host accepts a plugin when the
  major matches and the minor is `<=` the host's. `TESSERA_PLUGIN_ABI_VERSION ==
  0x00010000`.

Specification: [`docs/plugin-abi.md`](docs/plugin-abi.md). Normative header:
[`include/plugin_abi.h`](include/plugin_abi.h) (carries a compile-time freeze
assertion on the major version). Conformance of the in-tree reference plugins is
checked by `make verify-plugin-abi`.
