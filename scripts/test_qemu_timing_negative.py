#!/usr/bin/env python3
"""Compile real delay injections and prove the normal acceptance runner rejects them.

Never treats compiler errors, timeouts, unrelated failures or a PASS as a
successful negative control. Rebuilds uninjected binaries before returning.
"""
from pathlib import Path
import argparse
import json
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cross", default="aarch64-linux-gnu-")
    args = parser.parse_args()
    output = Path("build/arm/timing-negative")
    output.mkdir(parents=True, exist_ok=True)
    results = []
    clean = ["make", "-j1", "build-arm-budget-qemu", "build-arm-latency-qemu",
             f"CROSS_COMPILE={args.cross}", "TIMING_TEST_DEFS="]
    success = False
    try:
        for fixture,define,expected in [
            ("budget", "TIMING_INJECT_WORKER_DELAY", "worker=0"),
            ("latency", "TIMING_INJECT_SERVICE_DELAY", "no-overrun=0"),
        ]:
            with (output/f"{fixture}-build.log").open("w") as log:
                subprocess.run(["make", "-j1", f"build-arm-{fixture}-qemu",
                    f"CROSS_COMPILE={args.cross}", f"TIMING_TEST_DEFS=-D{define}"],
                    stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
            run = subprocess.run([sys.executable, "scripts/run_qemu_timing.py",
                    "--fixture", fixture, "--mode", "deterministic"],
                    capture_output=True, text=True, timeout=60)
            (output/f"{fixture}-runner.log").write_text(run.stdout+run.stderr)
            serial = Path(f"build/arm/virt_{fixture}.log").read_text()
            label = "BUDGET" if fixture == "budget" else "AUDIO-LAT"
            ok = (run.returncode == 1 and f"{label}: FAIL" in serial and
                  f"{label}: PASS" not in serial and expected in serial)
            results.append({"fixture":fixture,"define":define,"runner_exit":run.returncode,
                            "expected_failure_detected":ok})
            (output/"results.json").write_text(json.dumps(results,indent=2)+"\n")
            if not ok:
                raise RuntimeError(f"wrong or absent negative-control failure: {fixture}\n{serial}")
            print(f"negative control {fixture}: PASS (real delay rejected by unchanged assertions)", flush=True)
        success = True
    finally:
        with (output/"restore-uninjected.log").open("w") as log:
            subprocess.run(clean, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
    return 0 if success else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"negative-control gate failed: {exc}",file=sys.stderr)
        raise SystemExit(1)
