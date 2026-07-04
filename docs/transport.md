# Master transport and clock (Theme C, issue #114)

Time-aware effects - tempo-synced delay, LFOs, arpeggiators - need a shared
musical clock. The transport (`arch/arm64/transport.c`) is that clock: a tempo,
a time signature, a running bar/beat/tick position advanced from the audio-core
cadence, and play/stop. It is published to plugins each block as the ABI v1.1
transport snapshot (`tessera_transport_t`, issue #124), can free-run at its own
tempo or slave to an external MIDI clock, and can emit MIDI clock downstream.

## Model

- **Resolution:** `TP_PPQ` = 96 internal ticks per quarter note (divisible by
  24, so MIDI clock at 24 PPQN maps to whole ticks).
- **Position:** `bar` / `beat` / `tick`, where a beat is `TP_PPQ * 4 / den`
  ticks - a quarter in 4/4, an eighth in 6/8 - and there are `num` beats per bar.
- **Advance (`transport_advance`):** exact integer arithmetic,
  `ticks = n_frames * TP_PPQ * tempo_mbpm / (60000 * sr)` with a remainder
  accumulator, so the position never drifts and lands on boundaries precisely
  (1 s at 120 BPM is exactly 2 quarters). No floating point on the audio path.
- **Tempo:** milli-BPM (`120000` == 120 BPM).

## MIDI clock

- **In (`transport_midi_clock_in`):** each incoming `0xF8` steps one 24-PPQN
  tick and, from the frames since the previous clock, sets the tempo
  (`BPM = 60 * sr / (24 * frames)`); `0xFA`/`0xFB`/`0xFC` are start / continue /
  stop.
- **Out (`transport_clock_out`):** `transport_advance` accumulates one `0xF8`
  pulse every `TP_PPQ / 24` ticks; the driver drains them and sends the bytes.

The MIDI parser exposes `midi_is_realtime()` so the driver can route System
Real-Time bytes to the transport without disturbing a note message in flight.

## Tests

`make test-arm-transport` - host unit tests (ASan/UBSan): position advance at a
set tempo (single block and block-by-block, no drift), bar/beat accounting in
4/4 and 6/8, MIDI clock in (tempo estimate + position), MIDI clock out at
24 PPQN, start/continue/stop, and the plugin-facing snapshot.

The tempo-synced parameters and tap tempo (#115) and the arpeggiator (#116)
build on this transport; the host wires the snapshot into each plugin's ABI v1.1
event queue.
