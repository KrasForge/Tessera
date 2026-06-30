# Audio Callback Latency and Jitter (M4)

This is the measurement evidence required by the M4 "done when" criterion
(issue #22). It documents how Tessera measures the audio callback timing, what
the measurement points are, and the numbers obtained from the automated test
harness, with a section reserved for a real Compute Module 4 (CM4) run.

## What is measured

Two quantities are tracked on the dedicated audio core (CPU0, issue #21):

1. **Callback period** - the time between the start of one audio callback and
   the start of the next. At a nominal 1 kHz block rate this should be 1000 us;
   the deviation from 1000 us is the audio jitter.
2. **IRQ-to-thread wakeup latency** - the time from the timer/DMA deadline (when
   the interrupt condition becomes true) to the first instruction of the
   callback. This isolates interrupt-entry and wake-from-WFI overhead from the
   rest of the callback.

Both are derived from the 64-bit system counter `CNTPCT_EL0`, read at the very
start of every callback. The implementation lives in `arch/arm64/latency.c`:

- A ring of the most recent `LAT_WINDOW` (1000) inter-callback deltas.
- `min`, `max`, `mean`, and `standard deviation` over that window, computed in
  fixed-point integer math (the kernel builds `-mgeneral-regs-only`; there is no
  floating point on the audio path). The standard deviation uses an integer
  square root of the variance `E[x^2] - E[x]^2`.
- A separate running max/mean for the wakeup-latency metric.
- `overruns` come from the audio watchdog (`audio_core.c`, issue #21): a
  callback whose service time exceeds its budget.

## How it is reported

The stats line uses the exact format required by the issue:

```
audio_latency: min=Xus max=Yus mean=Zus stddev=Wus overruns=N
audio_wakeup:  max=Pus mean=Qus
```

Reporting must not perturb the audio core, because a UART write at 115200 baud
takes longer than a 1 ms callback period. The audio core therefore never writes
the UART during operation: every second it publishes a statistics snapshot via a
seqlock, and a **reporter running on a different core** (CPU1) renders the line.
The slow UART transfer happens entirely off the audio core's timeline, so it
cannot cause an overrun. The test confirms this: the reporter prints
continuously while CPU0 services every callback with zero overruns.

## Methodology / how to reproduce

```
# Pure statistics math (host, deterministic):
make test-arm-latency

# End-to-end on the emulated four-core machine:
make test-arm-latency-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

The host test (`tests/arm64/latency_test.c`) checks the min/max/mean/stddev,
the integer square root, and the overflow-safe cycles->microseconds conversion
against hand-computed expected values. The QEMU harness
(`tests/arm64/virt/latency_main.c`) runs the real audio-core, ring, watchdog,
and reporter across four cores.

## Results: QEMU `virt` (emulated, CI)

Captured from `make test-arm-latency-qemu` (Cortex-A72, `CNTFRQ` = 62.5 MHz,
1 kHz block rate, 3000 callbacks, reporter on CPU1, producer on CPU2):

```
audio_latency: min=4us  max=6458us mean=1000us stddev=217us overruns=0
audio_latency: min=10us max=2806us mean=1000us stddev=103us overruns=0
audio_latency: min=5us  max=6759us mean=1000us stddev=265us overruns=0
final: serviced=3000 underruns=0 overruns=0
final audio_wakeup: max=8180us mean=154us
```

The **mean period is exactly 1000 us**, i.e. the cadence is correct and no
callbacks are lost (3000 serviced, 0 underruns, 0 overruns).

The min/max/stddev spread is **not** representative of real hardware: QEMU's
multi-threaded TCG (`-smp 4`) interleaves the four vCPU host threads and is not
cycle-accurate, so individual `CNTPCT` deltas jump around by milliseconds. This
is why the automated test asserts correctness of the *mechanism* (cadence,
no underrun, no overrun, the reporter printing concurrently without corrupting
output) rather than the absolute 500 us jitter bound, which is a property of the
hardware, not the emulator.

## Results: Raspberry Pi CM4 (real hardware)

> To be captured on a board. The CI runners do not provide the BCM2711
> `raspi4b` QEMU machine (needs QEMU >= 9.0) or real hardware, so these numbers
> must come from a physical CM4 run of the same audio-core build.

Procedure on hardware:

1. Boot the kernel on the CM4 with the audio core pinned to CPU0.
2. Drive the audio callback from the PCM/I2S DMA completion interrupt
   (`audio_dma_irq`) at the real 48 kHz / block-size cadence.
3. Optionally toggle a GPIO at the start of the callback and at the ISR entry,
   and capture both with a logic analyser to cross-check the counter-derived
   wakeup latency.
4. Record the `audio_latency:` / `audio_wakeup:` lines over UART for an
   otherwise-idle system and again under load on CPU1-3.

| Condition            | min | max | mean | stddev | wakeup max | overruns |
|----------------------|-----|-----|------|--------|------------|----------|
| Idle                 | TBD | TBD | TBD  | TBD    | TBD        | TBD      |
| Load on CPU1-3       | TBD | TBD | TBD  | TBD    | TBD        | TBD      |

**Acceptance target:** max jitter below 500 us on an otherwise-idle system, and
the overrun counter at zero.
