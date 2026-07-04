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
