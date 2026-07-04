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
