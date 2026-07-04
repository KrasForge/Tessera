# Patches and presets

A **patch** captures a graph configuration so it can be saved to storage and
reloaded on the next boot: which plugins are loaded, how they are wired, and the
parameter values for each. Tessera stores a patch as a small line-based text
file, on the FAT SD card (or the ramdisk when testing).

## Syscalls

```c
long sys_patch_save(const char *path);   /* serialise live state to `path` */
long sys_patch_load(const char *path);   /* load plugins, restore wiring + params */
```

Both return `0` on success or a negative error. `sys_patch_save` is **atomic**:
it writes a temporary file and then renames it over the target, so a power cut
during a save cannot corrupt the previous patch. `sys_patch_load` on a corrupt
or truncated file returns an error and leaves the system running (it never
panics).

## File format

```
# tessera-patch v1
plugin effect                # plugin 0 (path passed to sys_plugin_load)
param 0 0 0x445c0000         # plugin 0, param id 0 = 880.0  (IEEE-754 hex)
connect input 0              # the audio input -> plugin 0 (issue #84)
connect 0 dac                # plugin 0 -> the DAC sink
```

- `plugin <path>` - loads an ELF by path; plugins are numbered 0, 1, ... in the
  order they appear.
- `param <plugin> <id> <value>` - a parameter for a plugin. Values are 32-bit
  IEEE-754 hex bit patterns (exact, and the kernel needs no floating point to
  read or write them). When editing by hand, a plain decimal integer is also
  accepted, e.g. `param 0 0 880`.
- `connect <src> <dst>` - a graph edge. `src` is a plugin number or `input` for
  the captured audio input; `dst` is a plugin number or `dac` for the audio
  output.
- Lines beginning with `#` are comments; blank lines are ignored.

The format is plain text so a developer can open a patch in an editor and change
a parameter, swap a plugin, or rewire the graph.

## Storage

Patches are written to the FAT volume on the SD card (`arch/arm64/fat.c`, which
gained write/rename support for this feature). For automated testing without a
card, the same `sys_patch_save`/`sys_patch_load` calls work against an in-memory
ramdisk store. Round-trip fidelity is bit-exact: saving a graph and reloading it
reproduces identical audio output (verified in `make test-arm-patch-qemu`).

## Scene / parameter morphing (Theme M17, issue #173)

Glitch-free patch switching (issue #103) crossfades the *audio* of two graphs;
scene morphing generalises that to the *parameter* space (`arch/arm64/morph.h`).
Given two patch snapshots and a single morph control (an expression pedal, a
tempo-synced LFO), it interpolates every parameter between them, so a performer
sweeps continuously from one sound to another instead of hard-switching.

Each parameter names an interpolation curve:

- **LINEAR** - levels, pan, mix.
- **EXP** - a geometric sweep for frequencies and times, so a cutoff morphs by
  equal musical intervals (200 → 3200 Hz is halfway at 800 Hz, two octaves, not
  the arithmetic 1700). Uses small self-contained exp2/log2, no libm.
- **STEP** - a discrete switch at the midpoint (waveform, mode).

A parameter present in only one snapshot **holds** its value, so two patches with
different plugins morph without error. `morph_value` gives one parameter's morphed
value; `morph_eval` fills the whole union of both snapshots at a position, which
the host streams onto the parameter queues (through the smoothers, so the sweep is
click-free). Like `param_map.h` this is control-plane float code - header-only, so
it compiles into the host/FP paths and never the `-mgeneral-regs-only` kernel.

Covered by `make test-arm-morph`.
