# Signal quality and I/O

Theme H hardens the parts of the platform that touch the real signal: denormal
protection, higher sample rates, and richer audio I/O.

## Denormal protection (issue #130)

Subnormal (denormal) floats are the numbers smaller than the smallest normal
float, represented with a zero exponent and a non-zero mantissa. On many FPUs an
operation that produces or consumes one is 10-100x slower than a normal op. In
audio DSP they appear all the time: a reverb tail or a filter's feedback state
decays smoothly toward zero and spends a long time in the subnormal range long
after the sound is inaudible. A plugin that is comfortably inside its CPU budget
with signal present can blow it in the silence *after* a note - the worst possible
time.

The platform guarantee is that **flush-to-zero is always on** when DSP runs. On
AArch64 that is `FPCR.FZ` (bit 24), which flushes subnormal inputs *and* results
to zero for single/double ops (the FTZ + DAZ behaviour). The scheduler seeds every
task's saved `FPCR` with `FZ` set (`arch/arm64/sched.c` via `denorm_fpcr_default`),
so a plugin inherits flush-to-zero the instant its FP context is restored - it
cannot forget to enable it, and there is no per-block cost.

`arch/arm64/denorm.h` also provides a software fallback that operates on float
*bit patterns* (so it is safe even in the integer-only kernel): `denorm_is_subnormal`
detects a subnormal and `denorm_flush` maps it to a correctly-signed zero, leaving
normals, zeros, and infinities untouched. It is useful for portability and as the
reference the tests check.

Covered by `make test-arm-denorm`.

## Sample-rate conversion / 96 kHz (issue #131)

The audio path runs at a fixed device rate, but sources and sinks may want
another: a plugin negotiated for 48 kHz feeding a 96 kHz DAC, a 44.1 kHz file
player, or USB audio (issue #133) at yet another rate. `arch/arm64/src.c` bridges
them - a streaming rational resampler that converts an int16 PCM stream from one
rate to another.

It is fixed-point: a **Q32 phase accumulator** steps through the input by
`in_rate / out_rate` samples per output, and each output is a **linear
interpolation** between the two straddling input samples. No floating point, so it
runs on the `-mgeneral-regs-only` audio path. Linear interpolation is exact at DC
and at the sample points and is cheap; a polyphase-FIR upgrade would tighten the
stopband without changing the interface.

The converter **streams**: its phase and last-sample state carry across
`src_process` calls, so a long stream resamples block-by-block with no seam - a
split input produces bit-identical output to the unsplit call. `src_out_capacity`
reports a safe output-buffer size for a given input length.

Covered by `make test-arm-src`.

## Multi-channel I/O (issue #132)

The graph carries multi-channel buses (issue #119); `arch/arm64/multiio.c` is the
device side - an interface with more than two channels (a multi-out DAC, a
four-in ADC) and the routing between the device's physical channels and the
graph's input/output bus channels.

Hardware moves frames **interleaved** (`L R L R ...`) while the graph and the DSP
work on **planar** per-channel buffers, so this module de-interleaves on capture
and interleaves on playback. Between the two it applies a **routing matrix**:

- `out_src[d]` - the graph channel feeding device output channel `d` (or
  `IO_SILENCE`). One graph channel can feed several device outputs (mono to a
  stereo pair), a pair can be swapped, or a channel left unpatched.
- `in_src[g]` - the device input channel feeding graph input channel `g` (or
  `IO_SILENCE`).

`io_config_init` starts from identity routing (graph channel *i* ↔ device channel
*i* where both exist, silence otherwise); `io_route_out` / `io_route_in` override
individual channels with bounds checks. A route to a missing or silenced source
outputs zeros rather than reading out of range. int16 PCM, integer only - it runs
on the audio path.

Covered by `make test-arm-multiio`.
