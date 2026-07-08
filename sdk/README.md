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
| `libtessera.a`         | Static helper library (built from `lib/`): `tessera_sinf`, `tessera_clampf`, `tessera_param_queue_read`, and the DSP building blocks. |
| `lib/`                 | Sources for `libtessera.a`. No libc, no allocation. |

### DSP building blocks

`libtessera.a` ships a small, real-time-safe DSP toolkit so you do not start from
`sinf` and a bare buffer (all allocation-free, no libc):

- **`tessera_smooth_*`** - a one-pole smoother for click-free parameter changes.
- **`tessera_biquad_*`** - RBJ biquads (low/high-pass, band-pass, notch, peaking,
  low/high shelf) with a per-sample `tessera_biquad_process`.
- **`tessera_osc_*`** - sine / saw / square / triangle oscillators (saw and square
  are polyBLEP anti-aliased).
- **`tessera_wt_*`** - a wavetable oscillator that plays a caller-supplied
  single-cycle table; give it a per-octave *mip stack* (each table band-limited to
  fewer harmonics) and it selects the alias-free table for the pitch.
  `tessera_wt_bandlimit` fills a band-limited sawtooth table.
- **`tessera_fm_op_*` / `tessera_fm2`** - phase-modulatable sine operators and a
  two-operator FM voice (ratio + index); `tessera_synth` exposes them as the
  `TESSERA_WAVE_FM` waveform via `tessera_synth_set_fm`.
- **`tessera_sampler_*`** - a streaming sampler: the host pushes source samples
  into a fixed ring off the audio path and the audio path pulls pitched,
  interpolated output. The ring is bounded, so memory never grows with the sample
  length (it cannot exceed the per-plugin memory quota); a refill that falls
  behind yields silence instead of stalling. Looping is fed by the host.
- **`tessera_delay_*`** - a fractional (interpolated) delay line over a
  caller-supplied buffer.
- **`tessera_envfollow_*`** - a peak envelope follower with attack/release.
- **`tessera_adsr_*`** - an ADSR envelope generator.

Declarations and doc comments are in [`tessera.h`](tessera.h); the blocks are
unit-tested by `make test-arm-dsp`.

### Reference effects suite

On top of those primitives, `libtessera.a` also ships a suite of complete
effects, so a pedal or channel-strip plugin can drop one in without wiring the
DSP by hand (still allocation-free and libc-free):

- **`tessera_fx_overdrive`** - a stateless soft-clipping waveshaper (drive + level).
- **`tessera_fx_comp_*`** - a feed-forward compressor (threshold, ratio, makeup,
  attack/release).
- **`tessera_fx_eq3_*`** - a 3-band EQ (low shelf, peaking mid, high shelf).
- **`tessera_fx_delay_*`** - a delay with feedback and a wet/dry mix (drive the
  delay time from the transport for tempo sync).
- **`tessera_fx_chorus_*`** - an LFO-modulated delay.
- **`tessera_fx_gate_*`** - a noise gate with a smoothed open/close.
- **`tessera_fx_reverb_*`** - a Schroeder reverb (4 damped combs into 2 allpasses).
- **`tessera_fx_tuner_*`** / **`tessera_fx_note_of`** - a zero-crossing pitch
  estimator with a Hz -> nearest-note/cents mapping.

Declarations and doc comments are in [`tessera.h`](tessera.h); the suite is
unit-tested by `make test-arm-fx`.

### IR convolution (cabinet sim)

- **`tessera_conv_*`** - a time-domain FIR that convolves the signal against an
  impulse response (a guitar cabinet, a room, a filter kernel). This is the
  deliberately *heavy* block: a few-thousand-tap IR costs real CPU every block,
  so a cabinet-sim plugin built on it exercises the M12 CPU budget and the
  process isolation under genuine load. Real-time safe - exactly `ir_len`
  multiply-adds per sample, and the caller owns both the IR and the history ring
  (`tessera_conv_normgain` gives the worst-case gain for pre-scaling).

Unit-tested by `make test-arm-conv`.

### Polyphonic synth engine

- **`tessera_synth_*`** - a voice allocator that turns ABI note events into
  audio, proving the synth-voice path end to end. Feed it decoded note events
  (`tessera_synth_event`) or call `tessera_synth_note_on/off` directly; each
  voice is one of the SDK oscillators through an ADSR. It allocates a free voice
  per note, steals the quietest under over-subscription, and reclaims a voice
  when its release finishes. `tessera_note_to_hz` maps a MIDI note to its
  frequency. Polyphony is set by the caller-supplied voice array.

Unit-tested by `make test-arm-synth`.

### FFT primitive

- **`tessera_fft_*`** - a radix-2 complex FFT and inverse (in place, power-of-two
  sizes) plus the packed real pair **`tessera_rfft`/`tessera_irfft`** (audio is
  real: n samples cost one n/2-point complex transform) and the periodic
  **`tessera_window_hann`/`_hamming`** helpers whose overlap-add identities STFT
  resynthesis relies on. Allocation-free: the caller owns the twiddle table
  (`tessera_fft_twiddles`, filled once at setup) and every buffer. The enabler
  for partitioned convolution, spectral effects, and spectrum/tuner analysis.

Unit-tested by `make test-arm-fft` (against a naive DFT reference).

### Partitioned FFT convolution (long IRs)

- **`tessera_pconv_*`** - uniform-partitioned overlap-save convolution: the IR is
  split into block-sized partitions transformed once at load, and each block
  costs one forward FFT, P bin-wise complex MACs, and one inverse FFT — bounded,
  position-independent work instead of `O(block × ir_len)`. Zero added latency
  and the *same filter* as `tessera_conv_*` (use the direct engine for short
  IRs, this for cabinets and rooms). The caller owns the spectra, the
  input-spectrum delay line, and the work frames.

Unit-tested by `make test-arm-pconv` (vs the direct engine and brute force).
| `Makefile`             | Builds `libtessera.a`. |
| `Makefile.template`    | Copy-pasteable makefile for your own plugin. |
| `examples/sine_plugin/`| A complete, commented example plugin built with only the SDK. |

The full ABI specification is [`../docs/plugin-abi.md`](../docs/plugin-abi.md).

## Writing plugins in Rust

Plugins can also be written in Rust: [`rust/tessera-plugin`](rust/tessera-plugin)
is a thin, safe wrapper over the same C ABI. Implement the `Plugin` trait over
slices and invoke the `tessera_plugin!` macro to emit the five ABI exports - the
pointer marshalling and instance lifecycle are handled for you. See its
[README](rust/tessera-plugin/README.md); tested with `make test-rust-sdk`.

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
