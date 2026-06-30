# effect_filter - reference low-pass filter plugin

A resonant low-pass filter for the Tessera audio graph (issue #29), and a
worked example for third-party plugin authors.  It implements the Tessera
plugin ABI (`include/plugin_abi.h`, issue #23) and is built against only that
header - it has no imports from the kernel.

The filter is a Chamberlin state-variable filter (a simple 2-pole IIR).
`process_block` is real-time-safe: a bounded loop of multiplies and adds over
static state, with no allocation, no locks, and no syscalls.

## ABI surface

| symbol                  | when                          | real-time safe |
|-------------------------|-------------------------------|----------------|
| `plugin_abi_version`    | first, before init            | yes            |
| `plugin_init`           | once at load                  | no             |
| `plugin_process_block`  | every audio block             | **yes**        |
| `plugin_set_param`      | from the audio path           | **yes**        |
| `plugin_destroy`        | once before unload            | no             |

## Parameters

| id | name        | meaning                          | default |
|----|-------------|----------------------------------|---------|
| 0  | cutoff_hz   | corner frequency in Hz           | 1000    |
| 1  | resonance_q | resonance (Q); clamped >= 0.5    | 0.707   |

`plugin_set_param(0, cutoff_hz)` updates a single coefficient and does not
touch the filter state, so changing the cutoff in real time is click-free
apart from the filter's own transient.

## Build

```
# Standalone AArch64 ELF (no kernel sources needed):
make plugins CROSS_COMPILE=aarch64-linux-gnu-
#   -> build/arm/plugin_effect_filter.elf

# Or directly:
aarch64-none-elf-gcc -mcpu=cortex-a72 -ffreestanding -fPIC -O2 -std=c11 \
    -I/path/to/Tessera/include -c main.c -o effect_filter.o
```

The DSP is unit-tested on the host with `make test-arm-filter` (frequency
response, real-time cutoff changes, and stability).
