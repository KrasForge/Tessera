#!/usr/bin/env python3
"""Render the README sound demos with the offline plugin host.

Every demo is produced by the real plugin C sources (plugins/synth_fm,
plugins/effect_filter, plugins/sampler) linked into tools/offline_host.c, the
same host-compiled path the golden-audio gate uses. The note and parameter
automation for each demo lives in docs/media/demos/*.csv, so any render can be
reproduced and diffed.

These are desktop renders, not recordings of a Raspberry Pi: QEMU has no I2S
block, and nothing has been played through a physical DAC yet.

    python3 scripts/render_demos.py            # MP3 + spectrogram PNG + MP4
    python3 scripts/render_demos.py --no-png   # skip the matplotlib step

Needs a host C compiler and make. MP3 encoding uses ffmpeg (skipped if
missing); spectrograms use numpy + matplotlib (skipped if missing).
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "arm")
DEMOS = os.path.join(ROOT, "docs", "media", "demos")
AUDIO = os.path.join(ROOT, "docs", "media", "audio")
IMAGES = os.path.join(ROOT, "docs", "media", "img")
RATE = 48000

# name -> (host binary, automation csv, seconds, input: None=silence or demo name)
DEMO_TABLE = [
    ("fm-bell",            "offline_host_synth",   "fm-bell.csv",      11.0, None),
    ("fm-bass",            "offline_host_synth",   "fm-bass.csv",       8.0, None),
    ("fm-bass-filter-sweep", "offline_host",       "filter-sweep.csv",  8.0, "fm-bass"),
    ("sampler-pitch",      "offline_host_sampler", "sampler-pitch.csv", 6.0, None),
]

TITLES = {
    "fm-bell": "synth_fm · Bell preset (ratio 3.5, index 5)",
    "fm-bass": "synth_fm · Bass preset (ratio 1.0, index 2)",
    "fm-bass-filter-sweep": "synth_fm → effect_filter · resonant low-pass sweep",
    "sampler-pitch": "sampler · pitch ratio 1.0 → 2.0 → 0.75 → 1.5, gated",
}


def run(cmd):
    subprocess.run(cmd, cwd=ROOT, check=True)


def write_silence(path, seconds):
    frames = int(seconds * RATE)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"\0\0\0\0" * frames)


def read_mono(path):
    with wave.open(path, "rb") as w:
        n, ch = w.getnframes(), w.getnchannels()
        raw = w.readframes(n)
    samples = struct.unpack("<%dh" % (n * ch), raw)
    return samples[::ch], w.getframerate()


def spectrogram(name, wav_path, png_path):
    try:
        import numpy as np
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("render_demos: numpy/matplotlib missing, skipping %s.png" % name)
        return
    x, sr = read_mono(wav_path)
    x = np.asarray(x, dtype=np.float32) / 32768.0
    t = np.arange(len(x)) / sr

    fig, (ax_w, ax_s) = plt.subplots(
        2, 1, figsize=(10, 4.2), dpi=110, sharex=True,
        gridspec_kw={"height_ratios": [1, 2.4], "hspace": 0.06})
    bg, fg, accent = "#0d1117", "#c9d1d9", "#58a6ff"
    fig.patch.set_facecolor(bg)
    for ax in (ax_w, ax_s):
        ax.set_facecolor(bg)
        ax.tick_params(colors=fg, labelsize=8)
        for s in ax.spines.values():
            s.set_color("#30363d")

    ax_w.plot(t, x, color=accent, linewidth=0.4)
    ax_w.set_ylim(-1, 1)
    ax_w.set_yticks([])
    ax_w.set_title(TITLES.get(name, name), color=fg, fontsize=10, loc="left")

    ax_s.specgram(x, NFFT=2048, Fs=sr, noverlap=1536, cmap="magma",
                  vmin=-130, vmax=-20)
    ax_s.set_ylim(0, 6000)
    ax_s.set_ylabel("Hz", color=fg, fontsize=8)
    ax_s.set_xlabel("seconds", color=fg, fontsize=8)
    ax_s.set_xlim(0, t[-1])
    fig.text(0.99, 0.01, "rendered by tools/offline_host.c · 48 kHz",
             color="#8b949e", fontsize=7, ha="right", va="bottom")
    fig.subplots_adjust(left=0.06, right=0.985, top=0.9, bottom=0.12)
    fig.savefig(png_path, dpi=110, facecolor=bg)
    # Pixel span of the time axis, so the MP4 playhead lines up with it.
    ext = ax_s.get_window_extent()
    plt.close(fig)
    return ext.x0, ext.x1, len(x) / sr


def video(png_path, mp3_path, mp4_path, span):
    """Spectrogram + audio + a moving playhead, for GitHub's inline player."""
    x0, x1, dur = span
    graph = ("[0:v]scale=trunc(iw/2)*2:trunc(ih/2)*2[bg];"
             "color=c=white@0.85:s=2x2000:r=25[head];"
             "[bg][head]overlay=x='%.1f+%.1f*t/%.3f':y=0,format=yuv420p[v]"
             % (x0, x1 - x0, dur))
    run(["ffmpeg", "-loglevel", "error", "-y", "-loop", "1", "-r", "25",
         "-i", png_path, "-i", mp3_path, "-filter_complex", graph,
         "-map", "[v]", "-map", "1:a", "-c:v", "libx264", "-tune", "stillimage",
         "-c:a", "aac", "-b:a", "160k", "-shortest", "-movflags", "+faststart",
         mp4_path])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--no-png", action="store_true", help="skip spectrograms")
    args = ap.parse_args()

    for d in (BUILD, AUDIO, IMAGES):
        os.makedirs(d, exist_ok=True)
    run(["make", "-s", "offline-host", "offline-host-synth", "offline-host-sampler"])
    have_ffmpeg = shutil.which("ffmpeg") is not None

    rendered = {}
    for name, host, csv, seconds, source in DEMO_TABLE:
        wav = os.path.join(AUDIO, name + ".wav")
        if source is None:
            src = os.path.join(BUILD, "demo-silence-%s.wav" % name)
            write_silence(src, seconds)
        else:
            src = rendered[source]
        run([os.path.join(BUILD, host), src, wav, os.path.join(DEMOS, csv)])
        rendered[name] = wav
        if have_ffmpeg:
            run(["ffmpeg", "-loglevel", "error", "-y", "-i", wav,
                 "-codec:a", "libmp3lame", "-qscale:a", "2", "-ac", "1",
                 os.path.join(AUDIO, name + ".mp3")])
        if not args.no_png:
            png = os.path.join(IMAGES, name + ".png")
            span = spectrogram(name, wav, png)
            if span and have_ffmpeg:
                video(png, os.path.join(AUDIO, name + ".mp3"),
                      os.path.join(AUDIO, name + ".mp4"), span)

    # The WAVs are intermediate; the committed demos are the MP3s.
    if have_ffmpeg:
        for wav in rendered.values():
            os.remove(wav)
    print("render_demos: wrote %d demos to %s" % (len(rendered),
                                                  os.path.relpath(AUDIO, ROOT)))


if __name__ == "__main__":
    sys.exit(main())
