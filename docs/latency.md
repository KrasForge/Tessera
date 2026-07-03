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

# Round-trip Latency (M14: live in, through an effect, out)

The audio-callback section above measures the *cadence* of one core. This
section measures the **round trip**: how long a single sample takes to travel
from the capture edge (the ADC/DMA finishing a block) all the way to the DAC
output, through the audio graph. For a guitar pedal the round-trip number *is*
the product spec, so it is measured with the counter, not estimated (issue #85).

## What is measured

Two points on the same `CNTPCT_EL0` basis as `arch/arm64/latency.c`:

1. **Capture edge** - the moment a captured block is handed to the software
   (produced into the I2S capture ring, `drivers/i2s_capture.c`, issue #83). A
   timestamp is recorded for that block.
2. **DAC output** - the moment the corresponding block is emitted to the DAC.
   The round trip is the difference of the two counter values.

The measurement tracks each block's capture timestamp out of band (a small FIFO
travelling in lockstep with the audio through the rings), so the correlation is
exact and does not depend on recovering a marker from the filtered audio. The
harness reports `min` / `max` / `mean` (and `stddev`) in microseconds, the same
shape as the callback stats above.

## Buffer accounting (the theoretical minimum)

The round trip is dominated by two one-block ring delays that sit *outside* the
graph compute, exactly where they sit in hardware. At the M14 geometry
(`RING_BLOCK` = 256 frames, `RING_SR` = 48 kHz) one block is 256 / 48000 =
**5.333 ms**.

| Stage                         | Boundary crossed        | Latency      |
|-------------------------------|-------------------------|--------------|
| Capture ring (ADC/DMA -> input node) | 1 block boundary | 1 block (5.333 ms) |
| Graph compute (input -> filter -> DAC) | same period, no boundary | ~0 (a few us) |
| DAC output ring (DAC node -> DAC/DMA) | 1 block boundary | 1 block (5.333 ms) |
| **Total (digital minimum)**   |                         | **2 blocks (10.667 ms)** |

So the predicted round trip is **2 blocks**. The graph compute runs within a
single period and its cost (microseconds) does not cross a block boundary, so it
does not add a block. On real hardware the analog ADC and DAC converters add
their own group delay (tens of samples of filter latency each), and a sample
captured mid-block waits up to one block for the next boundary; those are added
on top of this digital minimum when the CM4 numbers are captured below.

## How it is reported and reproduced

```
# End-to-end loopback on the emulated machine:
make test-arm-roundtrip-qemu CROSS_COMPILE=aarch64-linux-gnu-
```

The QEMU `virt` harness (`tests/arm64/virt/roundtrip_main.c`) builds the real
graph - the capture input node (issue #84) -> the reference low-pass plugin
(issue #29) -> the DAC - and wraps it in the two one-block ring delays above. A
**modelled loopback** feeds each DAC-output block back into the capture source
(the stand-in for a physical DAC-out-to-ADC-in cable), and a DC step injected at
the start gives a signal to trace end to end through the filter. Time is paced
by busy-waiting on `CNTPCT` to a per-block deadline, so each period lasts one
real block interval and the measured round trip lands at ~2 blocks.

The harness asserts the deterministic thing - the per-block delay is **exactly
2**, a zero-block error against the buffer-accounting prediction - and reports
the microsecond figure. It also checks the data path (the injected step comes
out of the DAC non-silent) and that no block is dropped or duplicated
(`overruns` = `underruns` = 0).

## Results: QEMU `virt` (emulated, CI)

Captured from `make test-arm-roundtrip-qemu` (Cortex-A72, `CNTFRQ` = 62.5 MHz,
256-frame blocks at 48 kHz, 64 blocks, single core, MMU on):

```
CNTFRQ=62500000 Hz, block=256 frames @ 48000 Hz -> 5333 us/block, predicted round trip = 2 blocks = 10666 us
first signal out at block 2 (delay=2 blocks)
roundtrip: min=9681us max=10592us mean=10551us stddev=125us samples=62
delay: exactly-2-blocks=62 off-by=0  (predicted 2 blocks)
capture: overruns=0 underruns=0  dac-sound-blocks=62
checks: data-path=1 accounting=1 clean=1 measured-in-1-block=1
```

Every measured block took **exactly 2 block periods** (a 0-block error against
the prediction), the DC step travelled input -> filter -> DAC and emerged
non-silent, and the counter-derived mean (~10.55 ms) sits within one block of
the 10.667 ms prediction. As with the callback stats, the absolute microsecond
spread is a property of the pacing, not cycle-accurate hardware; the harness
asserts the deterministic block count and reports the microseconds.

## Results: Raspberry Pi CM4 (real hardware)

> To be captured on a board with a physical DAC-out-to-ADC-in loopback cable.
> The CI runners provide neither the `raspi4b` QEMU machine nor real hardware,
> so these numbers must come from a physical CM4 run of the same audio build.

Procedure on hardware:

1. Boot the kernel on the CM4 with the audio graph running the reference
   low-pass: capture input -> filter -> DAC, at the real 48 kHz / 256-frame
   cadence driven by the PCM/I2S DMA completion interrupt.
2. Wire a **loopback cable** from the DAC/line output back to the ADC/line
   input, so the emitted audio is re-captured.
3. Inject a single click (or GPIO-toggled marker) into the capture stream, or
   drive the DAC output with a known impulse, and record `CNTPCT_EL0` at the
   capture edge and at the DAC block that carries the returned marker.
4. Optionally toggle a GPIO at both instants and capture with a logic analyser
   to cross-check the counter-derived round trip against wall-clock.
5. Record the `roundtrip:` line for an otherwise-idle system and again under
   load on CPU1-3.

| Condition            | min | max | mean | stddev | blocks | notes                 |
|----------------------|-----|-----|------|--------|--------|-----------------------|
| Idle                 | TBD | TBD | TBD  | TBD    | TBD    | + ADC/DAC group delay |
| Load on CPU1-3       | TBD | TBD | TBD  | TBD    | TBD    | + ADC/DAC group delay |

**Sanity check:** the measured round trip should be the 2-block digital minimum
(10.667 ms) plus the converters' analog group delay; a number far from that
points at an extra buffer in the path.
