# The Tessera serial shell (M13)

Tessera exposes an interactive control shell over the serial console. With it a
user builds and drives the audio graph, tweaks parameters live, and saves and
restores patches, without compiling any C. The shell is the human front end to
the same control plane the syscalls expose (issue #30) and the patch format
issue #40 defines.

- The shell **core** (issue #80) is the line editor, tokeniser, and command
  dispatcher: [`arch/arm64/shell.c`](../arch/arm64/shell.c).
- The **graph and patch commands** (issues #81/#82) are in
  [`arch/arm64/shell_graph.c`](../arch/arm64/shell_graph.c).

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

This is the M13 "no C" acceptance, verified end to end on QEMU by
`make test-arm-shell-patch-qemu`. A user builds a `synth -> filter -> DAC`
chain, saves it, reboots the board, reloads the patch, and hears exactly the
same audio - every step typed at the console, nothing recompiled.

```
tessera> load /rd/synth
loaded pid 1
tessera> load /rd/effect
loaded pid 2
tessera> wire 1 2
wired
tessera> wire 2 dac
wired
tessera> set-param 1 0 880
set
tessera> run
rendered one block; dac-hash 0x229df365
tessera> patch save /sd/live.patch
saved /sd/live.patch
tessera> reboot
rebooted (graph cleared; SD card intact)
tessera> patch ls
patches:
  LIVE.PAT
tessera> patch load /sd/live.patch
loaded /sd/live.patch
tessera> run
rendered one block; dac-hash 0x229df365
tessera> patch load /sd/missing.patch
error: patch load: no such file/pid
```

The `dac-hash` after reload is bit-for-bit identical to the one before the
reboot: the graph, its wiring, and the 880 Hz parameter all round-tripped
through the human-readable patch on the SD card (see
[`docs/patches.md`](patches.md) for the file format). Loading a patch that is
missing - or, in the M9 test, one that is truncated - is reported as an error
and the shell keeps running.

(`run` and `reboot` in the transcript are test-harness verbs that render a
block and model a power cycle; on hardware the audio engine renders
continuously and a reboot is a real one. Everything else is a production shell
command.)

## Sharing the UART

On the target the shell runs off the audio core (CPU0 is untouchable) and
shares the one UART with the periodic `audio_latency:` reporter. The platform
serialises whole messages with a lock, so shell responses and reporter lines
never tear on the wire (issue #80).
