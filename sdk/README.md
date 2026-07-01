# Tessera Plugin SDK

Everything an outside developer needs to build, load, and share a Tessera audio
plugin - using only this directory and a stock AArch64 bare-metal toolchain. No
Tessera kernel sources or headers are required.

## Contents

| Path | What it is |
| --- | --- |
| `tessera.h`            | The single header you include. Wraps the ABI and adds convenience macros and helper declarations. |
| `plugin_abi.h`         | The frozen Plugin ABI v1 header (bundled; identical to the kernel's `include/plugin_abi.h`). |
| `tessera.ld`           | Link script that lays out a plugin at the Tessera user base with separate R-X / R / R-W segments (W^X-safe). |
| `libtessera.a`         | Static helper library (built from `lib/`): `tessera_sinf`, `tessera_clampf`, `tessera_param_queue_read`. |
| `lib/`                 | Sources for `libtessera.a`. No libc, no allocation. |
| `Makefile`             | Builds `libtessera.a`. |
| `Makefile.template`    | Copy-pasteable makefile for your own plugin. |
| `examples/sine_plugin/`| A complete, commented example plugin built with only the SDK. |

The full ABI specification is [`../docs/plugin-abi.md`](../docs/plugin-abi.md).

## Requirements

- An AArch64 bare-metal GCC. The default toolchain triple is
  `aarch64-none-elf-`; override with `CROSS_COMPILE`, e.g.
  `CROSS_COMPILE=aarch64-linux-gnu-`.

## Build the example

```sh
cd examples/sine_plugin
make                                   # -> sine_plugin.elf
# or, with a different toolchain:
make CROSS_COMPILE=aarch64-linux-gnu-
```

This produces `sine_plugin.elf`: a self-contained AArch64 plugin. Copy it to the
SD card and load it at runtime with `sys_plugin_load("/sd/sine_plugin.elf")`.

## Write your own plugin

1. Copy `Makefile.template` next to your source as `Makefile`, and set `PLUGIN`
   to your source's basename and `SDK` to the path of this directory.
2. Include `<tessera.h>` and implement the five required exports (see
   [`examples/sine_plugin/sine_plugin.c`](examples/sine_plugin/sine_plugin.c)):

   ```c
   #include "tessera.h"

   TESSERA_DEFINE_ABI_VERSION()                       // plugin_abi_version

   TESSERA_PLUGIN_EXPORT int plugin_init(uint32_t sr, uint32_t bs) { ... }
   TESSERA_PLUGIN_EXPORT void plugin_process_block(
       const float *il, const float *ir, float *ol, float *orr, uint32_t n) { ... }
   TESSERA_PLUGIN_EXPORT void plugin_set_param(uint32_t id, float v) { ... }
   TESSERA_PLUGIN_EXPORT void plugin_destroy(void) { }
   ```

3. `make`. The result is a loadable plugin ELF.

## Real-time rules (short version)

`plugin_process_block` and `plugin_set_param` run on the audio thread: no
allocation, no syscalls, no blocking, no unbounded work. Do setup in
`plugin_init`. Your plugin can touch only its own memory and the audio buffers
the host passes it. See [`../docs/plugin-abi.md`](../docs/plugin-abi.md) §5-6.
