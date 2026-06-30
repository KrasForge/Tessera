# External hardware

Hardware needed to drive Tessera's live inputs and outputs on a Raspberry Pi
Compute Module 4 (BCM2711).  Audio output (PCM5102 DAC over I2S) is covered by
the M3 issues; this page covers the M7 control inputs.

## MIDI input (DIN-5, issue #31)

Standard opto-isolated MIDI IN on UART3 (GPIO bcm pins for UART3 RX), 31250
baud, driven by `drivers/midi_uart.c`.

- 5-pin DIN socket -> 6N138 (or H11L1) opto-isolator -> UART3 RX.
- MIDI is current-loop: pin 4 through ~220 ohm to +3.3 V, pin 5 to the
  opto-isolator input; do **not** connect the cable shield to ground at the IN.

## CV / Gate input (issue #32)

Eurorack-style control input.  The CM4 has no analog input, so Pitch CV is read
through an external SPI ADC, and the Gate is a digital GPIO input.

### Gate

- A 0/+5 V (or +10 V) Gate signal into a GPIO pin, level-shifted to the GPIO's
  0/3.3 V range (a resistor divider or a 74LVC-style level shifter; the GPIO
  pins are **not** 5 V tolerant).
- Rising edge -> Note On, falling edge -> Note Off (`cvgate_update`).

### Pitch CV via SPI ADC

| part            | role                                                   |
|-----------------|--------------------------------------------------------|
| MCP3208         | 8-channel, 12-bit SPI ADC                               |
| op-amp + dividers | scale/offset the CV (often -5..+5 V or 0..+10 V) into the ADC's 0..VREF input range |
| VREF            | the ADC reference (e.g. 4.096 V or 5.0 V)               |

- Wire the MCP3208 to **SPI0** (GPIO 7-11: CE1, CE0, MISO, MOSI, SCLK) at ALT0,
  driven by `drivers/spi.c`.
- Pitch CV (1 V/octave) goes through the conditioning op-amp into one ADC
  channel; `cv_to_note()` converts the 12-bit code to a MIDI note.
- **Calibration** (`cv_cal_t`): set `code_0v`, `codes_per_volt` (= codes per
  octave), and `base_note` to match your scaling network.  With a 12-bit ADC
  over a 0..5 V window that is ~819 codes/volt; tune `code_0v`/`codes_per_volt`
  against measured 0 V and 1 V references so each octave lands on the right
  note.

### Event flow

Both MIDI and CV/Gate produce the same `midi_event_t` (tagged `INPUT_SRC_MIDI`
or `INPUT_SRC_CV`) onto one lock-free event ring, so the host sees a single,
unified stream of note/CC events regardless of the physical source.
