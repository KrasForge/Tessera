#!/usr/bin/env python3
"""Interactive Tessera QEMU frontend with persistent FAT storage.

Kernel commands are sent to the actual PL011. :reboot and :quit flush the
emulated memory-backed FAT image through QMP before terminating the emulator.
This is an emulator frontend, not a physical SD/I2S implementation.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from session_console_transport import Console


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/arm"))
    parser.add_argument("--image", type=Path, default=Path("build/workstation-card.img"))
    parser.add_argument("--mode", choices=["deterministic", "wallclock"], default="deterministic")
    args = parser.parse_args()
    binary = (args.build_dir / "session/kernel.elf").resolve()
    row = next(line.split() for line in subprocess.check_output(
        ["aarch64-linux-gnu-nm", "-S", str(binary)], text=True).splitlines()
        if line.endswith(" session_sd"))
    address, size = int(row[0], 16), int(row[1], 16)
    image = args.image.resolve()
    image.parent.mkdir(parents=True, exist_ok=True)
    if image.exists() and image.stat().st_size != size:
        raise ValueError(f"Existing image has wrong size; preserve it and select a different --image: {image}")
    root = args.build_dir / "workstation-runs"
    root.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="run-", dir=root)).resolve()
    print(f"Storage: {image}\nTiming: {args.mode}; use :reboot or :quit to flush storage.")
    boot = 0
    while True:
        boot += 1
        console = Console(binary, run, f"boot-{boot}", image if image.exists() else None,
                          address, args.mode, 0)
        reboot = False
        try:
            while True:
                try:
                    command = input("tessera> ")
                except (EOFError, KeyboardInterrupt):
                    command = ":quit"
                if command.strip() in (":quit", ":reboot", "quit"):
                    console.send("pause")
                    pending = image.with_name(image.name + ".tmp")
                    console.rpc("pmemsave", {"val": address, "size": size, "filename": str(pending)})
                    if pending.stat().st_size != size:
                        raise OSError("Incomplete storage export; old image retained")
                    os.replace(pending, image)
                    console.finish()
                    reboot = command.strip() == ":reboot"
                    break
                console.proc.stdin.write((command + "\n").encode())
                console.proc.stdin.flush()
                print(console.read_prompt(), end="")
        finally:
            console.close()
        if not reboot:
            break
    print(f"Storage saved. Serial logs: {run}")


if __name__ == "__main__":
    main()
