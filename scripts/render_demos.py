#!/usr/bin/env python3
"""Render the README sound demos from the in-tree reference plugins.

Every sound is produced by the plugins' own C (plugins/synth_fm and
plugins/effect_filter, on the SDK's libtessera DSP) driven through
tools/offline_host.c, which calls the same five ABI entry points the kernel
does. This is the desktop path, not QEMU and not hardware: QEMU has no I2S
block, and nothing here was recorded from a board.

After the offline host writes its 16-bit WAV, the only processing is one fixed
gain to -1 dBFS peak and MP3 / AAC encoding. There is no EQ, compression, reverb
or mixing. The synth -> filter demo is two offline-host passes, exactly the
`wire 1 2; wire 2 dac` graph from the shell.

Needs a host C compiler, ffmpeg with libmp3lame, and Python 3 with numpy and
matplotlib.

    python3 scripts/render_demos.py     # -> docs/media/demo-*.mp3 and *.png

It also writes build/demos/demo-*.mp4: the dark spectrogram with a moving
playhead over the same audio. GitHub only plays video inline when it is uploaded
through its web editor (a user-attachments URL), so those are for uploading.
"""
from __future__ import annotations

import argparse
import math
import subprocess
import wave
from pathlib import Path

import numpy as np

SR = 48000
ROOT = Path(__file__).resolve().parent.parent

# Figure geometry, shared by the spectrogram plot and the playhead video.
FIG_W, FIG_H, DPI = 8, 3.1, 200
LEFT, RIGHT, TOP, BOTTOM = 0.075, 0.985, 0.80, 0.14

# synth_fm parameter ids (plugins/synth_fm/main.c)
NOTE_ON, NOTE_OFF, FM_RATIO, FM_INDEX, ATTACK, DECAY, SUSTAIN, RELEASE = range(8)
# effect_filter parameter ids (plugins/effect_filter/main.c)
CUTOFF, RESONANCE = 0, 1


class Score:
    """Builds an offline-host automation CSV (`frame,param_id,value`)."""

    def __init__(self, bpm: float):
        self.bpm = bpm
        self.events: list[tuple[int, int, float]] = []

    def frame(self, beat: float) -> int:
        return round(beat * 60.0 / self.bpm * SR)

    def at(self, beat: float, param: int, value: float) -> None:
        self.events.append((self.frame(beat), param, value))

    def note(self, beat: float, length: float, midi: int) -> None:
        self.at(beat, NOTE_ON, midi)
        self.at(beat + length, NOTE_OFF, midi)

    def write(self, path: Path) -> None:
        # Stable sort: events on the same frame keep the order they were added.
        rows = sorted(self.events, key=lambda e: e[0])
        path.write_text("".join(f"{f},{p},{v!r}\n" for f, p, v in rows))


def write_silence(path: Path, seconds: float) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"\0\0\0\0" * round(seconds * SR))


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
        return data.reshape(-1, w.getnchannels())[:, 0].astype(np.float64) / 32768.0


# ---- the demos -------------------------------------------------------------

def bell() -> tuple[Score, float]:
    """synth_fm's factory "Bell" preset (ratio 3.5, index 5) over Am-F-C-G."""
    s = Score(bpm=96)
    for p, v in ((FM_RATIO, 3.5), (FM_INDEX, 5.0), (ATTACK, 2.0), (DECAY, 900.0),
                 (SUSTAIN, 0.0), (RELEASE, 1200.0)):
        s.at(0, p, v)
    chords = [(57, [69, 72, 76, 81]), (53, [65, 69, 72, 77]),
              (60, [67, 72, 76, 79]), (55, [67, 71, 74, 79])]
    order = [0, 1, 2, 3, 2, 1, 2, 3]
    for bar, (root, tones) in enumerate(chords):
        s.note(bar * 4, 3.5, root)
        for step, idx in enumerate(order):
            s.note(bar * 4 + step * 0.5, 0.45, tones[idx])
    for midi in (57, 64, 69, 76):  # four notes: stays inside the 8-voice pool
        s.note(16, 2, midi)
    return s, 16 * 60 / 96 + 1.4


def bass() -> tuple[Score, float]:
    """synth_fm's factory "Bass" preset (ratio 1, index 2), FM index swept 0.5 -> 6 -> 0.5."""
    s = Score(bpm=120)
    for p, v in ((FM_RATIO, 1.0), (FM_INDEX, 2.0), (ATTACK, 2.0), (DECAY, 180.0),
                 (SUSTAIN, 0.5), (RELEASE, 60.0)):
        s.at(0, p, v)
    roots = [45, 41, 48, 43]
    pattern = {0: 0, 3: 12, 6: 0, 8: 0, 10: 12, 11: 0, 14: 7}
    steps = len(roots) * 16
    for i in range(steps):
        tri = 1.0 - abs(2.0 * i / (steps - 1) - 1.0)
        s.at(i * 0.25, FM_INDEX, round(0.5 + 5.5 * tri, 3))
        if i % 16 in pattern:
            s.note(i * 0.25, 0.2, roots[i // 16] + pattern[i % 16])
    return s, 16 * 60 / 120 + 0.5


def pad() -> tuple[Score, float]:
    """synth_fm as a slow pad (ratio 2, index 1.2); the input to the filter pass."""
    s = Score(bpm=120)
    for p, v in ((FM_RATIO, 2.0), (FM_INDEX, 1.2), (ATTACK, 300.0), (DECAY, 400.0),
                 (SUSTAIN, 0.55), (RELEASE, 800.0)):
        s.at(0, p, v)
    chords = [[57, 64, 72], [53, 60, 69], [48, 55, 64], [55, 62, 71]]
    for bar, tones in enumerate(chords):
        for midi in tones:
            s.note(bar * 4, 3.9, midi)
    return s, 16 * 60 / 120 + 1.0


def filter_sweep(seconds: float) -> tuple[Score, np.ndarray]:
    """effect_filter at Q 2.5: cutoff swept 180 Hz -> 6 kHz -> 180 Hz, one update per block."""
    s = Score(bpm=60)  # one beat = one second
    s.at(0, RESONANCE, 2.5)
    sweep = 8.0
    curve = []
    for frame in range(0, round(seconds * SR), 256):
        t = frame / SR
        tri = 1.0 - abs(2.0 * min(t, sweep) / sweep - 1.0)
        hz = round(180.0 * (6000.0 / 180.0) ** tri, 1)
        s.events.append((frame, CUTOFF, hz))
        curve.append((t, hz))
    return s, np.array(curve)


# ---- rendering ---------------------------------------------------------------

def run(cmd: list[str]) -> None:
    subprocess.run(cmd, cwd=ROOT, check=True, stdout=subprocess.DEVNULL)


def offline(host: Path, src: Path, dst: Path, score: Score, work: Path) -> None:
    csv = work / (dst.stem + ".csv")
    score.write(csv)
    run([str(host), str(src), str(dst), str(csv)])


def encode_mp3(wav: Path, mp3: Path) -> float:
    peak = float(np.max(np.abs(read_wav(wav))))
    gain_db = -1.0 - 20.0 * math.log10(peak)
    run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(wav), "-ac", "1",
         "-af", f"volume={gain_db:.2f}dB", "-codec:a", "libmp3lame", "-q:a", "2",
         "-map_metadata", "-1", str(mp3)])
    return gain_db


def encode_video(wav: Path, png: Path, mp4: Path, gain_db: float, seconds: float) -> None:
    """The spectrogram image with a playhead sweeping its time axis, over the clip."""
    w, h = round(FIG_W * DPI), round(FIG_H * DPI)
    x0, span = LEFT * w, (RIGHT - LEFT) * w
    y0, y1 = round((1 - TOP) * h), round((1 - BOTTOM) * h)
    graph = (f"color=c=white@0.85:s=3x{y1 - y0}:r=30,format=rgba[head];"
             f"[0:v][head]overlay=x='{x0:.1f}+{span:.1f}*t/{seconds:.4f}':y={y0},"
             f"format=yuv420p[v];"
             f"[1:a]volume={gain_db:.2f}dB,aformat=channel_layouts=mono[a]")
    run(["ffmpeg", "-loglevel", "error", "-y", "-loop", "1", "-framerate", "30",
         "-i", str(png), "-i", str(wav), "-filter_complex", graph,
         "-map", "[v]", "-map", "[a]", "-t", f"{seconds:.4f}",
         "-c:v", "libx264", "-preset", "slow", "-crf", "26", "-c:a", "aac", "-b:a", "160k",
         "-movflags", "+faststart", "-map_metadata", "-1", str(mp4)])


THEMES = {
    # Surfaces and ink from the dataviz reference palette; the spectrogram is a
    # one-hue (blue) sequential ramp that recedes toward each theme's surface.
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", muted="#52514e", grid="#e4e3df",
                  wave="#2a78d6", accent="#eb6834",
                  ramp=["#fcfcfb", "#cde2fb", "#86b6ef", "#3987e5", "#1c5cab", "#0d366b"]),
    "dark": dict(surface="#1a1a19", ink="#ffffff", muted="#c3c2b7", grid="#33332f",
                 wave="#3987e5", accent="#d95926",
                 ramp=["#1a1a19", "#0d366b", "#1c5cab", "#3987e5", "#86b6ef", "#cde2fb"]),
}


def plot(audio: np.ndarray, title: str, subtitle: str, out: Path, theme: str,
         cutoff: np.ndarray | None = None) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.colors import LinearSegmentedColormap

    th = THEMES[theme]
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 8,
                         "axes.edgecolor": th["grid"], "axes.labelcolor": th["muted"],
                         "xtick.color": th["muted"], "ytick.color": th["muted"]})
    fig = plt.figure(figsize=(FIG_W, FIG_H), dpi=DPI, facecolor=th["surface"])
    gs = fig.add_gridspec(2, 1, height_ratios=[1, 3], hspace=0.08,
                          left=LEFT, right=RIGHT, top=TOP, bottom=BOTTOM)
    ax_w, ax_s = fig.add_subplot(gs[0]), fig.add_subplot(gs[1])
    dur = len(audio) / SR

    # Waveform as a min/max envelope per column.
    cols = 1400
    edges = np.linspace(0, len(audio), cols + 1).astype(int)
    lo = np.array([audio[a:b].min() for a, b in zip(edges[:-1], edges[1:])])
    hi = np.array([audio[a:b].max() for a, b in zip(edges[:-1], edges[1:])])
    t = np.linspace(0, dur, cols)
    ax_w.fill_between(t, lo, hi, color=th["wave"], linewidth=0)
    lim = max(1e-6, np.max(np.abs(audio))) * 1.08
    ax_w.set_ylim(-lim, lim)
    ax_w.set_yticks([])
    ax_w.set_xticks([])

    # Log-frequency spectrogram (2048-point Hann STFT, hop 256), 65 dB range.
    n, hop = 2048, 256
    win = np.hanning(n)
    pad_audio = np.concatenate([np.zeros(n // 2), audio, np.zeros(n // 2)])
    frames = 1 + (len(pad_audio) - n) // hop
    idx = np.arange(n)[None, :] + hop * np.arange(frames)[:, None]
    mag = np.abs(np.fft.rfft(pad_audio[idx] * win, axis=1)).T
    db = 20 * np.log10(mag / mag.max() + 1e-12)
    freqs = np.fft.rfftfreq(n, 1 / SR)
    times = np.arange(frames) * hop / SR
    keep = (freqs >= 30) & (freqs <= 16000)
    cmap = LinearSegmentedColormap.from_list("seq", th["ramp"])
    ax_s.pcolormesh(times, freqs[keep], db[keep], cmap=cmap, vmin=-65, vmax=0,
                    shading="auto", rasterized=True)
    ax_s.set_yscale("log")
    ax_s.set_ylim(30, 16000)
    ax_s.set_yticks([50, 100, 200, 500, 1000, 2000, 5000, 10000])
    ax_s.set_yticklabels(["50", "100", "200", "500", "1k", "2k", "5k", "10k"])
    ax_s.minorticks_off()
    ax_s.set_ylabel("Hz")
    ax_s.set_xlabel("seconds")
    if cutoff is not None:
        ax_s.plot(cutoff[:, 0], cutoff[:, 1], color=th["accent"], linewidth=1.2)
        i = int(len(cutoff) * 0.30)
        ax_s.annotate("filter cutoff", (cutoff[i, 0], cutoff[i, 1]), xytext=(-62, 10),
                      textcoords="offset points", color=th["ink"], fontsize=7.5,
                      fontweight="bold")
    for ax in (ax_w, ax_s):
        ax.set_facecolor(th["surface"])
        ax.set_xlim(0, dur)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)

    fig.text(LEFT, 0.93, title, color=th["ink"], fontsize=10.5, fontweight="bold")
    fig.text(LEFT, 0.855, subtitle, color=th["muted"], fontsize=8)
    fig.savefig(out, facecolor=th["surface"])
    plt.close(fig)
    # 256-colour palette PNG: visually identical here, about a fifth of the size.
    from PIL import Image
    Image.open(out).convert("RGB").quantize(colors=256, method=Image.Quantize.FASTOCTREE,
                                            dither=Image.Dither.NONE).save(out, optimize=True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "media")
    ap.add_argument("--work", type=Path, default=ROOT / "build" / "demos")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    args.work.mkdir(parents=True, exist_ok=True)

    run(["make", "--no-print-directory", "offline-host", "offline-host-synth"])
    synth_host = ROOT / "build" / "arm" / "offline_host_synth"
    filter_host = ROOT / "build" / "arm" / "offline_host"

    renders = []
    for name, build, title, subtitle in (
        ("demo-bell", bell, "synth_fm  ·  factory “Bell” preset",
         "2-operator FM, ratio 3.5, index 5  ·  Am–F–C–G arpeggio, 96 BPM"),
        ("demo-bass", bass, "synth_fm  ·  factory “Bass” preset, FM index sweep",
         "2-operator FM, ratio 1  ·  index automated 0.5 → 6 → 0.5, 120 BPM"),
    ):
        score, seconds = build()
        silence = args.work / f"{name}-in.wav"
        write_silence(silence, seconds)
        wav = args.work / f"{name}.wav"
        offline(synth_host, silence, wav, score, args.work)
        renders.append((name, wav, title, subtitle, None))

    score, seconds = pad()
    silence = args.work / "demo-chain-in.wav"
    write_silence(silence, seconds)
    dry = args.work / "demo-chain-dry.wav"
    offline(synth_host, silence, dry, score, args.work)
    fscore, curve = filter_sweep(seconds)
    wet = args.work / "demo-chain.wav"
    offline(filter_host, dry, wet, fscore, args.work)
    renders.append(("demo-chain", wet, "synth_fm → effect_filter  ·  two plugins in series",
                    "FM pad (ratio 2, index 1.2) into the resonant SVF low-pass, Q 2.5", curve))

    for name, wav, title, subtitle, cutoff in renders:
        gain = encode_mp3(wav, args.out / f"{name}.mp3")
        audio = read_wav(wav)
        for theme in THEMES:
            plot(audio, title, subtitle, args.out / f"{name}-{theme}.png", theme, cutoff)
        encode_video(wav, args.out / f"{name}-dark.png", args.work / f"{name}.mp4", gain,
                     len(audio) / SR)
        print(f"{name}: {len(audio) / SR:.1f} s, gain {gain:+.1f} dB -> {args.out / name}.mp3,"
              f" {args.work / name}.mp4")


if __name__ == "__main__":
    main()
