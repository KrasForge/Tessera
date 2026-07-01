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
plugin synth                 # plugin 0 (path passed to sys_plugin_load)
plugin effect                # plugin 1
param 0 0 0x445c0000         # plugin 0, param id 0 = 880.0  (IEEE-754 hex)
connect 0 1                  # plugin 0 -> plugin 1
connect 1 dac                # plugin 1 -> the DAC sink
```

- `plugin <path>` - loads an ELF by path; plugins are numbered 0, 1, ... in the
  order they appear.
- `param <plugin> <id> <value>` - a parameter for a plugin. Values are 32-bit
  IEEE-754 hex bit patterns (exact, and the kernel needs no floating point to
  read or write them). When editing by hand, a plain decimal integer is also
  accepted, e.g. `param 0 0 880`.
- `connect <src> <dst>` - a graph edge; `dst` is a plugin number or `dac` for
  the audio output.
- Lines beginning with `#` are comments; blank lines are ignored.

The format is plain text so a developer can open a patch in an editor and change
a parameter, swap a plugin, or rewire the graph.

## Storage

Patches are written to the FAT volume on the SD card (`arch/arm64/fat.c`, which
gained write/rename support for this feature). For automated testing without a
card, the same `sys_patch_save`/`sys_patch_load` calls work against an in-memory
ramdisk store. Round-trip fidelity is bit-exact: saving a graph and reloading it
reproduces identical audio output (verified in `make test-arm-patch-qemu`).
