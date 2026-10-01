# README media: where every screenshot and sound comes from

Everything in this directory comes from running the code in this repository. Nothing
here was recorded on a Raspberry Pi. QEMU has no BCM2711 I2S block, and the hardware
bring-up ([#105](https://github.com/KrasForge/Tessera/issues/105)–[#108](https://github.com/KrasForge/Tessera/issues/108))
is still open.

## Sound demos (`audio/`, `img/*-*.png`, `demos/`)

Rendered by [`scripts/render_demos.py`](../../scripts/render_demos.py)
(`make render-demos`). The script builds [`tools/offline_host.c`](../../tools/offline_host.c)
three times, once linked against each of the [`synth_fm`](../../plugins/synth_fm),
[`effect_filter`](../../plugins/effect_filter) and [`sampler`](../../plugins/sampler)
sources. It then drives each plugin through its ABI entry points in 256-frame blocks at
48 kHz. No mastering, EQ or normalisation is applied: the MP3s are the plugin output,
LAME-encoded at VBR quality 2.

| File | Plugin | Input | Automation |
| --- | --- | --- | --- |
| `fm-bell` | `synth_fm` | silence (a synth ignores its input) | [`demos/fm-bell.csv`](demos/fm-bell.csv) |
| `fm-bass` | `synth_fm` | silence | [`demos/fm-bass.csv`](demos/fm-bass.csv) |
| `fm-bass-filter-sweep` | `effect_filter` | the `fm-bass` render | [`demos/filter-sweep.csv`](demos/filter-sweep.csv) |
| `sampler-pitch` | `sampler` | silence (plays its bundled loop) | [`demos/sampler-pitch.csv`](demos/sampler-pitch.csv) |

The automation files use the offline host's `frame,param_id,value` format. The
parameter IDs are documented in each plugin's README:

- `synth_fm`: 0 = note-on (MIDI note), 1 = note-off, 2 = FM ratio, 3 = FM index,
  4/5/6/7 = attack / decay / sustain / release (ms; sustain 0..1).
- `effect_filter`: 0 = cutoff (Hz), 1 = resonance Q.
- `sampler`: 0 = pitch ratio, 1 = gate.

Each `.mp4` is the matching spectrogram with a playhead and the same audio. GitHub
won't play audio or video stored in a repository inline. For an inline player, drag
an `.mp4` into a GitHub issue or the README web editor, and use the
`user-attachments` URL it creates.

## Screenshots (`img/fault-containment.png`, `img/budget-enforcement.png`, `img/workstation-session.png`)

These are styled terminal renders of the logs in [`transcripts/`](transcripts/), which
are committed verbatim (line endings normalised). The logs were captured on 2026-10-01
under `qemu-system-aarch64` (QEMU `virt`, Cortex-A72, `aarch64-linux-gnu-gcc`):

| Screenshot | Command | Note |
| --- | --- | --- |
| `fault-containment.png` | `make test-arm-resilience-qemu` | Shows cycle 1 in full, then the cycle-10 result and the final summary. The fault details of cycles 2–10 are elided in the image only; [`transcripts/resilience.txt`](transcripts/resilience.txt) has all ten. |
| `budget-enforcement.png` | `make test-arm-budget-qemu` | Complete. |
| `workstation-session.png` | `make build-arm-session SESSION_DEFS="-DSESSION_FRAMES=240 -DSESSION_RATE=12000"`, then the same `qemu-system-aarch64` line as `make run-arm-workstation` | The commands were typed into the PL011 console by a script. This is the 12 kHz / 240-sample functional profile that `test-arm-session-console` uses. At the 48 kHz / 64-sample product profile, the same session shows deadline misses under emulation. [The Verification section of the main README](../../README.md#verification) explains why QEMU timing is not treated as hardware evidence either way. |

Timings, PIDs, allocator counts and hashes vary between builds and host machines.
