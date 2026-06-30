# example_gain - reference Tessera plugin

A minimal stereo gain plugin that implements the Tessera plugin ABI
(`include/plugin_abi.h`, issue #23).  It exists to demonstrate that the ABI is
usable and self-contained: it is written against only `plugin_abi.h` and needs
no Tessera kernel sources.

## Build

```
# Cross-compile to an AArch64 relocatable ELF (what an in-kernel loader relocates):
aarch64-none-elf-gcc -mcpu=cortex-a72 -ffreestanding -fPIC -O2 -std=c11 \
    -I/path/to/Tessera/include -c gain.c -o gain.o

# Or via the repo's test target, which also checks the exported symbols:
make test-arm-plugin-elf CROSS_COMPILE=aarch64-linux-gnu-
```

## ABI surface

| symbol                  | when                          | real-time safe |
|-------------------------|-------------------------------|----------------|
| `plugin_abi_version`    | first, before init            | yes            |
| `plugin_init`           | once at load                  | no (may alloc) |
| `plugin_process_block`  | every audio block             | **yes**        |
| `plugin_set_param`      | from the audio path           | **yes**        |
| `plugin_destroy`        | once before unload            | no             |

This plugin understands one parameter, `PARAM_GAIN` (id 0): a linear gain
applied to both channels, default 1.0.
