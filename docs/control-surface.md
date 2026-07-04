# Control surface (the pedal front panel)

Tessera is meant to live in a stompbox, so the physical front panel - footswitches,
rotary encoders, an expression pedal - and any attached MIDI controller all need to
reach plugin parameters. `arch/arm64/ctlmap.c` is the mapping layer that turns those
control events into parameter updates (Theme E, issue #120).

## Sources and modes

A control is identified by a `(type, id)` pair:

| Source            | `ctl_src_type_t`  | Value                         |
|-------------------|-------------------|-------------------------------|
| Footswitch        | `CTL_SRC_FOOTSW`  | `>= 64` = down, else up       |
| Rotary encoder    | `CTL_SRC_ENCODER` | absolute position `0..127`    |
| Expression pedal  | `CTL_SRC_EXPR`    | `0..127`                      |
| MIDI Control Change | `CTL_SRC_MIDI_CC` | the CC value `0..127`       |

Each binding maps one source onto a plugin parameter's integer range
`[out_min, out_max]` with a mode:

- **`CTL_MODE_CONTINUOUS`** - scale `0..127` linearly onto the range (expression
  pedals, encoders, knob-style CCs). Out-of-range input is clamped.
- **`CTL_MODE_MOMENTARY`** - down reports `out_max`, up reports `out_min` (a
  hold-to-engage footswitch).
- **`CTL_MODE_TOGGLE`** - each *press* flips between `out_min` and `out_max`; the
  release is ignored (a latching bypass footswitch).

The layer is integer-only (no floating point), so a binding can be applied on the
`-mgeneral-regs-only` audio path. It is the real-time-safe sibling of the float,
host-only `param_map.h` (issue #33), which remains for CC-only host control.

## MIDI-learn

Rather than typing a CC number, the user arms *learn* for a parameter and then
moves the control they want to assign:

```c
ctlmap_learn_begin(&map, param_id, 0, 127, CTL_MODE_CONTINUOUS);
/* ... next control event that arrives ... */
ctlmap_feed(&map, src, raw, &param, &value);  /* binds src, and reports value */
```

The first `ctlmap_feed` after arming binds the incoming control to the armed
parameter (replacing any prior binding for that control), disarms learn, and
reports the parameter's value straight away - so the assigning "wiggle" also takes
effect immediately. `ctlmap_learn_cancel` disarms without binding.

## Applying updates

`ctlmap_feed` returns `1` with the target `param_id` and scaled `value` when the fed
control is bound; the host then enqueues that on the plugin's lock-free parameter
queue (issue #30), exactly as MIDI CC control does today. A latching footswitch only
reports on the press edge, so the queue sees one update per press, not two per
press/release.

Covered by `make test-arm-ctlmap`.

## On-device OLED UI (issue #121)

The other half of the front panel is the little screen. `arch/arm64/oled_ui.c` is
the UI *model and layout*: a pure state machine that renders the current screen
into a fixed 21x8 character grid, which the display driver blits through its font.
Only the pixel font and the SSD1306/SH1106 bring-up are hardware; everything about
*what the screen shows* and *how the buttons navigate* is pure and host-tested.

Three screens, driven by four buttons (up / down / select / back):

- **Home** - the title, live **CPU** and **headroom** bar meters (fed per-mille
  from the profiler, issue #129, and drawn with `oled_ui_bar` as `[####----]`),
  and the name of the currently loaded patch.
- **Patches** - a scrolling list of patch names; up/down move the selection (it
  wraps, and the view scrolls to keep it visible), select loads the patch and
  opens its parameters, back returns home.
- **Params** - a scrolling list of the loaded patch's parameters with their
  values, right-aligned; back returns to the patch list.

`oled_ui_render` fills the grid with space-padded ASCII (the selected row is
marked with `>` and may additionally be inverted by the driver). Integer-only, no
allocation - it can run wherever the display task lives.

Covered by `make test-arm-oled-ui`.

## Program change and patch banks (issue #122)

A live rig switches patches from the floor, so Tessera maps MIDI **Program Change**
onto stored patches. `arch/arm64/bank.c` is the routing model: patches are grouped
into *banks* (folders on the SD card), and each program in a bank is a patch file.
Populate the model by scanning the card (`bank_add`, `bank_set_program`), then feed
it MIDI events:

- **Bank Select** - CC 0 (MSB) and CC 32 (LSB) latch a pending bank number
  (`MSB*128 + LSB`). Bank Select only latches; it does not load on its own.
- **Program Change** - commits any latched Bank Select, then selects that program
  within the current bank. If the slot holds a patch, `bank_midi` returns its path
  and the host loads it (via `patch_mgr`, issue #40); an empty slot loads nothing.

The MIDI parser now emits Program Change as `MIDI_PROGRAM` (it was previously
parsed but dropped). The selected bank persists across Program Changes until the
next Bank Select, and an out-of-range Bank Select is ignored rather than switching
to a bank that does not exist.

Covered by `make test-arm-bank`.

## Desktop / remote editor over OSC (issue #123)

Patches can also be built from a laptop over USB serial or OSC. `arch/arm64/osc.c`
is the wire format: an Open Sound Control 1.0 message codec (address pattern +
type-tag string + big-endian arguments) plus a thin dispatch that turns editor
messages into the same parameter and graph edits the on-device shell performs:

| Address                | Type tags | Command                                  |
|------------------------|-----------|------------------------------------------|
| `/tessera/param`       | `,iii` / `,iif` | set plugin/param to a value (bit pattern) |
| `/tessera/connect`     | `,ii`     | connect src -> dst                       |
| `/tessera/disconnect`  | `,ii`     | disconnect src -> dst                    |
| `/tessera/load`        | `,s`      | load a patch file                        |
| `/tessera/save`        | `,s`      | save the current patch                   |
| `/tessera/ping`        | (none)    | liveness check                           |

OSC carries 32-bit floats, but the kernel builds `-mgeneral-regs-only` (no FP), so
a float argument is handled as its raw 32-bit IEEE-754 **bit pattern** (type `f`,
kept in a `uint32`) - never as a float, and no arithmetic is done on it - exactly
as the patch file format stores parameter values. The `,iii` form passes the same
bit pattern as an int, so a host with no float support can drive it too.

The parser is fed untrusted bytes from an external tool, so every field is
bounds-checked against the buffer: packets must be 4-byte aligned, the address
must start with `/` and be NUL-terminated within bounds, and a truncated or
over-long argument list is rejected rather than read past the end.

Covered by `make test-arm-osc`.
