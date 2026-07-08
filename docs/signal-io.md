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
and at the sample points and is cheap.

The converter **streams**: its phase and last-sample state carry across
`src_process` calls, so a long stream resamples block-by-block with no seam - a
split input produces bit-identical output to the unsplit call. `src_out_capacity`
reports a safe output-buffer size for a given input length.

Covered by `make test-arm-src`.

### Polyphase-FIR upgrade (issue #192)

Linear interpolation aliases on large ratios and rolls off the top octave -
measurably: a tone at 0.8 of the input Nyquist droops **-3.7 dB** through a 2x
upsample and its spectral image is only **12 dB** down; a 15 kHz tone
downsampled 48 -> 24 kHz aliases to 9 kHz **unattenuated**. `arch/arm64/
src_fir.c` is the product-grade upgrade with the same streaming interface
shape: a **Blackman-windowed sinc prototype** (32 input samples wide) split
into **32 polyphase sub-filters**, with the fractional position selecting two
adjacent phases whose coefficients are linearly interpolated.

- Everything is integer, generation included: the sinc/window tables are built
  at init by a self-contained Q15 integer sine, so the module builds and runs
  under `-mgeneral-regs-only`.  Per output: 32 exact Q30 MACs.
- The cutoff is 0.92 of the **narrower** Nyquist, so downsampling rejects
  would-be aliases (the anti-alias filter scales with the ratio) and
  upsampling rejects images.
- Every phase's taps are normalised to sum to exactly 32768 - and the phase
  interpolation is kept unrounded in Q30 - so **DC is bit-exact**, matching
  the linear SRC.

Measured against the linear SRC on the same inputs (`make test-arm-src-fir`):
passband **-0.13 dB** at 0.8 Nyquist (linear -3.68), image rejection
**67.6 dB** (linear 12.3), downsampling alias at **-60.6 dBFS** (linear
passes it through at full level), matching output counts, and bit-identical
chunked streaming.  Use `src_t` where cheap-and-cheerful is fine; use
`src_fir_t` where the audio is the product.

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

## USB audio (issue #133)

A class-compliant USB DAC/ADC streams PCM over an isochronous endpoint. Two parts
of that are pure logic, independent of the USB host controller, and live in
`arch/arm64/usbaudio.c`.

**Format discovery.** A device describes its stream in a USB Audio Class (UAC1)
Format Type I descriptor - channel count, bytes per sample, bit resolution, and
the discrete sample rates it supports (each a 24-bit little-endian value).
`usb_audio_parse_format` walks the descriptor set and extracts it. The bytes come
off the wire from an untrusted device, so the walk bounds-checks every field: a
zero or oversized `bLength`, a descriptor running past the buffer, or a format
that claims more sample rates than its length holds are all rejected rather than
over-read (a zero `bLength` cannot loop forever).

**Isochronous rate framing.** USB delivers one packet per (micro)frame at a fixed
frame rate, but the audio rate rarely divides it evenly - 44100 Hz over 1000
frames/s is 44.1 samples per frame. `usb_iso_next` uses the same exact fractional
accounting as the transport: it carries a remainder so the endpoint sends 44 or 45
samples each frame and 1000 frames deliver *exactly* 44100 samples. An integer
ratio (48000/1000) is a constant 48; high-speed microframes (96000/8000) a
constant 12.

Covered by `make test-arm-usbaudio`.
