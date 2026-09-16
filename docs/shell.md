# The Tessera serial shell (M13)

Tessera exposes an interactive control shell over the serial console. With it a
user builds and drives the audio graph, tweaks parameters live, and saves and
restores patches, without compiling any C. The shell is the human front end to
the same control plane the syscalls expose (issue #30) and the patch format
issue #40 defines.

- The shell **core** (issue #80) is the line editor, tokeniser, and command
  dispatcher: [`arch/arm64/shell.c`](../arch/arm64/shell.c).
- The original **graph and patch command** issue fixtures (#81/#82) use
  [`arch/arm64/shell_graph.c`](../arch/arm64/shell_graph.c). The production
  multicore workstation registers the corresponding managed commands in
  [`arch/arm64/session_shell.c`](../arch/arm64/session_shell.c), so serial
  control goes through the same frame-boundary session engine as M11.

## Commands

| Command | Meaning |
| --- | --- |
| `help` | list every command |
| `load <path>` | load a plugin ELF (e.g. `/sd/sine.elf`, `/rd/synth`); prints its pid |
| `unload <pid>` | unload a plugin and free its resources |
| `wire <src> <dst>` | connect an edge; `dst` is a pid or `dac` for the output |
| `unwire <src> <dst>` | remove an edge |
| `set-param <pid> <id> <value>` | set a plugin parameter live; `value` is a decimal (converted to float) or `0xHEX` raw bits |
| `ls` | list the graph: nodes (with names and params) and edges |
| `stats` | show the audio summary and per-plugin service times |
| `patch save <path>` | serialise the graph, wiring, and params to a file |
| `patch load <path>` | rebuild the graph from a saved patch |
| `patch ls` | list patch files on the SD card |

Bad input never faults the kernel: an unknown command, a missing or malformed
argument, or a backend failure (bad path, ABI mismatch, unknown node, corrupt
patch) each prints a single-line error and leaves the graph untouched.

## Session: build, tweak, save, reboot, reload

`make -j1 test-arm-m13 CROSS_COMPILE=aarch64-linux-gnu-` is the complete M13
software gate. It retains the original issue fixtures — including the real-UART
#80 reporter-interleaving test, the #81 `synth -> filter -> DAC` console build,
and the #82 console save/reboot/reload test — and also drives the production
multicore workstation through two independent QEMU processes.

The production cold-boot runner uses an explicitly declared 12 kHz / 240-sample
(20 ms) QEMU functional profile and executes this flow over the real PL011
console. The longer emulation frame separates shell/session correctness from
host-scheduler timing; every missing/skipped frame and plugin offence is still
an acceptance failure. The 48 kHz / 64-sample stress diagnostic remains
separate.


```text
tessera> load /sd/SOURCE.ELF
loaded pid 1
tessera> load /sd/GAIN.ELF
loaded pid 2
tessera> wire 1 2
ok
tessera> wire 2 dac
ok
tessera> contract 1 hard 2000us deadline=17000us policy=mute
ok
tessera> contract 2 hard 2000us deadline=19500us policy=mute
ok
tessera> pin 1 2
tessera> pin 2 1
tessera> set-param 2 0 0.5
tessera> start
tessera> wait 128
tessera> inspect
tessera> patch save /sd/LIVE.TSP
tessera> patch load /sd/MISSING.TSP
error: file not found or storage I/O error
tessera> pause
tessera> patch ls
patches:
  LIVE.TSP
```

The test exports only the FAT-backed storage region, exits the emulator, starts
a new emulator process with no preserved process memory, then continues:

```text
tessera> patch load /sd/LIVE.TSP
tessera> start
tessera> wait 64
tessera> inspect
```

The restored PCM hash and samples must be bit-identical to the first boot. The
runner then injects malformed, partially loadable, and incompatible session
files; each `patch load` must emit exactly one error line, retain the running
graph and keep producing the reference audio. A live `set-param` after restore
must change the real plugin output, and `ls` must show the current parameter bit
pattern. `patch ls` lists session files only, not the plugin ELF inventory.

`wait`, `inspect`, and `quit` are QEMU observation/lifecycle commands. They are
not needed to build, edit, save, or restore a patch on a physical system. The
storage in this acceptance test is a memory-backed FAT image; physical SD and
I2S acceptance remain part of M10.

## Sharing the UART

The original #80 QEMU fixture keeps CPU0 exclusively on audio, runs the shell on
CPU1, and emits periodic `audio_latency:` lines from CPU2 through a whole-message
UART lock. Its acceptance check proves the reporter and interactive responses do
not tear while CPU0 records zero overruns.

The production M11/M13 workstation reserves CPU3 for serial control and keeps
file I/O, ELF parsing and UART output off the audio IRQ. Runtime statistics are
available on demand through `stats`; DSP workers remain on CPU1/2 in the
interactive profile.

## `prof` - per-plugin profiler (issue #129)

`prof` reports where the CPU goes, aggregating the M12 per-plugin service-time
snapshot (`arch/arm64/plugin_time.c`) into per-plugin load and graph headroom
(`arch/arm64/profiler.c`):

```
prof: pid=3 runs=2400 mean=334us max=352us load=33.4% overruns=2
prof: pid=7 runs=2400 mean=120us max=131us load=12.0% overruns=0
prof: total load=45.4% headroom=54.6%
```

`load` is the mean service time as a fraction of the block period (per-mille
internally, shown as a percentage); `headroom` is `100% - total load`. The same
numbers feed the on-device meter (Theme E OLED UI, #121). The aggregation is
pure integer and off the audio path; covered by `make test-arm-profiler`.
