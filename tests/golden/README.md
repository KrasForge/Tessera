# Golden-audio regression (M16, #168)

Each reference plugin is rendered over a **fixed, internally generated
deterministic input** (two tones plus a seeded LCG noise) with a committed
automation script, and the output is compared against a committed reference
render. A DSP change that alters the sound moves the output far beyond the
tolerance and fails CI; tiny floating-point differences between compilers stay
well within it.

- `*.csv` — per-plugin automation (`frame,param_id,value`), the same format the
  offline host uses.
- `*.pcm` — the committed reference render (16384 × int16, left channel).

The comparison metric is the **RMS of the sample-by-sample difference relative to
the reference's RMS** (`tools/golden_check.c`) — "did the sound change" — with a
2% ceiling. A real algorithm change shifts this by whole percent; compiler jitter
is orders of magnitude smaller.

## Commands

```
make test-arm-golden    # render each reference plugin and compare (CI gate)
make golden-bless        # regenerate the *.pcm references after an intentional change
```

Re-bless only when you *meant* to change the sound, and review the diff.
