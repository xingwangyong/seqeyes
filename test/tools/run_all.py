#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path
import argparse

def run(cmd):
    print("+", " ".join(map(str, cmd)))
    return subprocess.run(cmd, text=True).returncode

def main():
    ap = argparse.ArgumentParser(description="Run all tests")
    ap.add_argument("--bin-dir", help="Directory containing built executables (SeqEyes and tests)")
    ap.add_argument("--seq-dir", help="Directory containing .seq files (for perf test override)")
    ap.add_argument("--out", help="Perf results JSON output (default: test/perf_results.json)")
    ap.add_argument("--baseline", help="Perf baseline JSON (default: test/perf_baseline.json if exists)")
    ap.add_argument("--threshold-ms", help="Perf regression threshold in ms; if omitted, 10% of baseline")
    args, _ = ap.parse_known_args()

    print("Select test to run:")
    print("  1) Load all .seq (headless)")
    print("  2) Zoom/Pan behavior (QtTest) on all .seq")
    print("  3) Perf: zoom timing on all .seq (compare baseline)")
    print("  4) Run all (1+2+3)")
    choice = input("Enter 1/2/3/4 (default 4): ").strip() or "4"

    repo = Path(__file__).resolve().parents[2]
    rc = 0
    if choice in ("1","4"):
        cmd = [sys.executable, str(repo/"test"/"test_load_all.py")]
        if args.bin_dir:
            cmd += ["--bin-dir", args.bin_dir]
        r = run(cmd)
        rc = rc or r
    if choice in ("2","4"):
        cmd = [sys.executable, str(repo/"test"/"test_zoom_pan.py")]
        if args.bin_dir:
            cmd += ["--bin-dir", args.bin_dir]
        r = run(cmd)
        rc = rc or r
    if choice in ("3","4"):
        cmd = [sys.executable, str(repo/"test"/"test_perf_zoom.py"), "--bin-dir", args.bin_dir or "."]
        if args.seq_dir:
            cmd += ["--seq-dir", args.seq_dir]
        # wire optional outputs/baseline/thresholds if provided
        if args.out:
            cmd += ["--out", args.out]
        if args.baseline:
            cmd += ["--baseline", args.baseline]
        if args.threshold_ms:
            cmd += ["--threshold-ms", args.threshold_ms]
        r = run(cmd)
        rc = rc or r
    sys.exit(rc)

if __name__ == "__main__":
    main()
