#!/usr/bin/env python3
"""Strict timing-fixture runner. Every repetition must pass; never retry failures.

Deterministic mode checks guest timing under an explicit 8 ns/instruction model,
not Cortex-A72 WCET. Wallclock mode retains MTTCG and identical assertions.
QEMU documentation: https://www.qemu.org/docs/master/devel/tcg-icount.html
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

FIXTURES = {
    "multicore": ("m11/kernel.elf", 4, "MULTICORE: PASS"),
    "budget": ("virt_budget.elf", 2, "BUDGET: PASS"),
    "latency": ("virt_latency.elf", 4, "AUDIO-LAT: PASS"),
    "temporal": ("virt_temporal.elf", 2, "TEMPORAL: PASS"),
    "temporal-acceptance": ("temporal-acceptance/kernel.elf", 2, "TEMPORAL ACCEPTANCE: PASS"),
}


def validate_result(returncode: int, text: str, marker: str) -> str | None:
    """A PASS substring alone is not success: process and final verdict matter."""
    if returncode != 0:
        return f"emulator exit {returncode}"
    lines = [line.strip() for line in text.splitlines()]
    if marker not in lines:
        return f"missing exact verdict: {marker}"
    if any(": FAIL" in line or "PANIC" in line for line in lines):
        return "failure/panic present despite PASS marker"
    if lines.count(marker) != 1 or lines[-1] != marker:
        return "ambiguous or non-final PASS marker"
    return None


def command(qemu: str, binary: Path, log: Path, fixture: str,
            mode: str, shift: int) -> list[str]:
    cores = FIXTURES[fixture][1]
    cmd = [qemu, "-machine", "virt", "-cpu", "cortex-a72", "-smp", str(cores),
           "-m", "256M", "-display", "none", "-serial", f"file:{log}",
           "-net", "none", "-kernel", str(binary)]
    if mode == "deterministic":
        cmd += ["-accel", "tcg,thread=single", "-icount",
                f"shift={shift},align=off,sleep=off"]
    elif mode == "wallclock":
        cmd += ["-accel", "tcg,thread=multi"]
    else:
        raise ValueError(f"unsupported timing mode: {mode}")
    return cmd


def positive(value: str) -> int:
    n = int(value)
    if n <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return n


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", choices=FIXTURES, required=True)
    parser.add_argument("--mode", choices=["deterministic", "wallclock"], default="deterministic")
    parser.add_argument("--shift", type=int, choices=range(0, 11), default=3)
    parser.add_argument("--repeat", type=positive, default=1)
    parser.add_argument("--timeout", type=positive, default=45)
    parser.add_argument("--build-dir", type=Path, default=Path("build/arm"))
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    args = parser.parse_args(argv)
    qemu = shutil.which(args.qemu)
    binary = (args.build_dir / FIXTURES[args.fixture][0]).resolve()
    if not qemu or not binary.is_file():
        parser.error(f"missing emulator or fixture binary: {args.qemu}, {binary}")
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    log_root = args.build_dir.resolve() / "timing-runs"
    log_root.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix=f"{args.fixture}-{args.mode}-", dir=log_root))
    version = subprocess.run([qemu, "--version"], capture_output=True, text=True,
                             timeout=10, check=True).stdout.splitlines()[0]
    report = {"fixture": args.fixture, "mode": args.mode, "qemu": version,
              "binary": str(binary), "binary_sha256": binary_hash,
              "shift": args.shift if args.mode == "deterministic" else None,
              "requested_runs": args.repeat, "runs": []}
    print(f"timing mode={args.mode} fixture={args.fixture}; evidence={output}", flush=True)
    for number in range(1, args.repeat + 1):
        log = output / f"run-{number:03d}.log"
        err = output / f"run-{number:03d}.stderr"
        cmd = command(qemu, binary, log, args.fixture, args.mode, args.shift)
        start = time.monotonic()
        with err.open("wb") as stderr:
            try:
                proc = subprocess.run(cmd, stdout=stderr, stderr=stderr,
                                      timeout=args.timeout, check=False)
                code = proc.returncode
            except subprocess.TimeoutExpired:
                code = 124
        text = log.read_text(errors="replace") if log.exists() else ""
        problem = validate_result(code, text, FIXTURES[args.fixture][2])
        if hashlib.sha256(binary.read_bytes()).hexdigest() != binary_hash:
            problem = "binary changed during verification"
        report["runs"].append({"run": number, "command": cmd, "exit": code,
            "passed": problem is None, "error": problem,
            "host_seconds": round(time.monotonic() - start, 6), "serial_log": str(log)})
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        # Keep conventional legacy log locations for existing tooling.
        conventional = args.build_dir / f"virt_{args.fixture.replace('-', '_')}.log"
        if log.exists():
            shutil.copyfile(log, conventional)
        print(f"{args.fixture} {args.mode} {number}/{args.repeat}: "
              + ("PASS" if problem is None else f"FAIL ({problem})"), flush=True)
        if problem is not None:
            print(text + err.read_text(errors="replace"), file=sys.stderr)
            return 1
        if args.repeat == 1:
            print(text, end="" if text.endswith("\n") else "\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"timing runner error: {exc}", file=sys.stderr)
        raise SystemExit(1)
