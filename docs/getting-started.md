# Getting started: your first Tessera plugin

This guide takes you from an empty machine to a running audio plugin in about
an hour. You will install a toolchain, build the example plugin from the SDK,
and run it - either in QEMU (no hardware needed) or on a Raspberry Pi CM4.

You do **not** need the Tessera kernel sources to write a plugin - only the SDK
in [`sdk/`](../sdk) and this guide. When you are ready to go beyond the example,
the full contract is in the [Plugin ABI reference](plugin-abi.md).

**Contents**

1. [Install the toolchain](#1-install-the-toolchain)
2. [Get the SDK](#2-get-the-sdk)
3. [Build the example plugin](#3-build-the-example-plugin)
4. [Run it in QEMU (no hardware)](#4-run-it-in-qemu-no-hardware)
5. [Run it on a Raspberry Pi CM4](#5-run-it-on-a-raspberry-pi-cm4)
6. [Next steps](#6-next-steps)
7. [Common errors](#7-common-errors)

---

## 1. Install the toolchain

A Tessera plugin is a bare-metal AArch64 binary, so you need an AArch64
cross-compiler. Either of these works:

- **Arm GNU bare-metal toolchain** (the SDK default), which provides
  `aarch64-none-elf-gcc`. Download it from the
  [Arm GNU Toolchain downloads](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  (pick the *AArch64 bare-metal target (aarch64-none-elf)* build for your host)
  and put its `bin/` directory on your `PATH`.

- **Debian/Ubuntu package** (what Tessera's CI uses):

  ```sh
  sudo apt-get update
  sudo apt-get install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
  ```

  This provides `aarch64-linux-gnu-gcc`. Because the packaged triple differs
  from the SDK default, pass `CROSS_COMPILE=aarch64-linux-gnu-` to every `make`
  below.

To also test without hardware (step 4), install QEMU's AArch64 system emulator:

```sh
sudo apt-get install -y qemu-system-arm   # provides qemu-system-aarch64
```

Check the compiler is on your `PATH`:

```sh
aarch64-none-elf-gcc --version        # or: aarch64-linux-gnu-gcc --version
```

## 2. Get the SDK

Clone the Tessera repository (or copy the `sdk/` directory into your own
project - it is self-contained):

```sh
git clone https://github.com/KrasForge/Tessera.git
cd Tessera
```

Everything a plugin author needs is under [`sdk/`](../sdk):

- [`sdk/tessera.h`](../sdk/tessera.h) - the single header you include.
- `sdk/libtessera.a` - DSP/util helpers (built on demand).
- [`sdk/tessera.ld`](../sdk/tessera.ld) - the plugin link script.
- [`sdk/examples/sine_plugin/`](../sdk/examples/sine_plugin) - the example.
- [`sdk/Makefile.template`](../sdk/Makefile.template) - a starter makefile for
  your own plugin.

See [`sdk/README.md`](../sdk/README.md) for the full layout.

## 3. Build the example plugin

The example is a stereo sine generator that uses only the SDK. Build it:

```sh
cd sdk/examples/sine_plugin
make                                     # uses aarch64-none-elf- by default
```

If you installed the Debian package, use the packaged triple instead:

```sh
make CROSS_COMPILE=aarch64-linux-gnu-
```

This produces **`sine_plugin.elf`**. Verify it is a valid, self-contained
plugin:

```sh
aarch64-none-elf-readelf -h sine_plugin.elf | grep AArch64
aarch64-none-elf-readelf -sW sine_plugin.elf | \
    grep -E 'plugin_(abi_version|init|process_block|set_param|destroy)$'
aarch64-none-elf-readelf -sW sine_plugin.elf | awk '$7=="UND" && $8!=""'   # empty
```

You should see an `AArch64` machine, all five `plugin_*` symbols, and **no**
output from the last command (no undefined imports).

> The exact build-and-verify sequence above is also captured in
> [`scripts/build-example-plugin.sh`](../scripts/build-example-plugin.sh), which
> Tessera's CI runs verbatim on every change so this guide stays correct.

## 4. Run it in QEMU (no hardware)

From the repository root, load and exercise the plugin you just built on the
emulated `virt` board:

```sh
make test-arm-sdk-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

This boots a small Tessera test image that loads `sine_plugin.elf` into an
isolated sandbox, runs its `process_block`, and checks that it produces audio
and responds to a parameter change. A successful run ends with:

```
SDK: PASS
QEMU virt SDK plugin test PASSED
```

That is your plugin running under Tessera, with no hardware involved.

## 5. Run it on a Raspberry Pi CM4

On real hardware the plugin is loaded from the SD card:

1. Copy the ELF to the FAT partition of the CM4's SD card:

   ```sh
   cp sine_plugin.elf /media/$USER/TESSERA/      # your SD mount point
   ```

2. Boot the CM4 with a Tessera image (see
   [`docs/build-arm.md`](build-arm.md) and [`docs/hardware.md`](hardware.md)).

3. Load the plugin at runtime by path:

   ```c
   sys_plugin_load("/sd/sine_plugin.elf");
   ```

   The host validates the binary, maps it into an isolated address space, checks
   the ABI version, and starts driving it from the audio engine. You should hear
   the 440 Hz tone.

## 6. Next steps

- Read the **[Plugin ABI reference](plugin-abi.md)** - the complete, frozen
  specification: the five exports, calling convention, real-time rules, the
  sandbox, and the versioning policy.
- Copy [`sdk/Makefile.template`](../sdk/Makefile.template) next to your own
  source, `#include "tessera.h"`, implement the five exports, and `make`.
- Study [`sdk/examples/sine_plugin/sine_plugin.c`](../sdk/examples/sine_plugin/sine_plugin.c),
  which is commented end to end and shows every SDK helper.
- Drive your plugin from the **[serial shell](shell.md)** - `load`, `wire`,
  `set-param`, and `patch save`/`load` let you build, tune, and persist a graph
  from the console without recompiling.

## 7. Common errors

### Wrong ABI version
**Symptom:** the host rejects the plugin at load, before any of your code runs.
**Cause:** `plugin_abi_version()` returned a value whose **major** version does
not match the host's.
**Fix:** always return the SDK's version:

```c
TESSERA_DEFINE_ABI_VERSION()          // returns TESSERA_ABI_VERSION
```

Do not hardcode a number. See [ABI reference §7](plugin-abi.md#7-versioning).

### Missing `TESSERA_PLUGIN_EXPORT`
**Symptom:** the host reports it cannot find a required symbol (e.g.
`plugin_init`), or the plugin fails to load.
**Cause:** an ABI function was optimised away or left with non-default
visibility, so it is not in the symbol table the host resolves by name.
**Fix:** mark every one of the five exports with `TESSERA_PLUGIN_EXPORT` (and
use `TESSERA_DEFINE_ABI_VERSION()` for the version function). Match the exact
signatures in [ABI reference §2](plugin-abi.md#2-required-exports).

### Non-PIC compile
**Symptom:** link errors about relocations (e.g. `R_AARCH64_*` "relocation
truncated to fit"), or a binary that misbehaves once mapped.
**Cause:** the plugin was compiled without `-fPIC`.
**Fix:** compile with `-fPIC -ffreestanding` (the SDK makefiles already do
this). Do not link against libc or any shared library; a plugin must be
self-contained with no undefined imports (verified in step 3).

### `SVC` (syscall) in `process_block`
**Symptom:** the plugin is killed the moment it processes audio, with a logged
`[sandbox] illegal SVC` message.
**Cause:** the plugin issued a system call from its own code. A sandboxed plugin
may reach the kernel only through the host's controlled entry path; any `SVC`
from the plugin body is a protocol violation.
**Fix:** never make system calls. Do all setup in `plugin_init`, and keep
`plugin_process_block` and `plugin_set_param` to pure computation - no
allocation, no syscalls, no blocking. See
[ABI reference §5-6](plugin-abi.md#5-the-sandbox-what-a-plugin-may-touch).
