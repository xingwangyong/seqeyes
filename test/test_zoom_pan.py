#!/usr/bin/env python3
import subprocess
from pathlib import Path
import sys
import argparse
import os

DEFAULT_CASES = [
    "writeEpi.seq",
    "writeGradientEcho.seq",
    "gre2d_pTxSingleChan_8Tx.seq",
]

def detect_test_exe(bin_dir: str | None = None) -> str:
    exe_name = "TimeSliderSyncTest.exe" if os.name == "nt" else "TimeSliderSyncTest"
    if bin_dir:
        # Prefer root bin dir, then common subfolder 'test'
        bin_path = Path(bin_dir)
        for p in [bin_path/exe_name, bin_path/"test"/exe_name]:
            if p.exists():
                return str(p)
        # MSVC multi-config generators put them in e.g. out/build/x64-Release/test/Release/
        for p in [bin_path.parent/"test"/"Release"/exe_name, 
                  bin_path.parent/"test"/"Debug"/exe_name,
                  bin_path.parent/"test"/bin_path.name/exe_name]:
            if p.exists():
                return str(p)
    candidates = [
        Path("out/build/Release")/exe_name,
        Path("out/build/Debug")/exe_name,
        Path("out/build")/exe_name,
        Path(exe_name),
    ]
    for c in candidates:
        if c.exists():
            return str(c)
    return exe_name

def main():
    ap = argparse.ArgumentParser(description="Zoom/Pan QtTest runner")
    ap.add_argument("--bin-dir", help="Directory containing built executables (TimeSliderSyncTest)")
    ap.add_argument(
        "--all",
        action="store_true",
        help="Run against every top-level .seq under test/seq_files instead of the curated CI subset",
    )
    ap.add_argument(
        "--timeout",
        type=int,
        default=120,
        help="Per-sequence timeout in seconds",
    )
    args = ap.parse_args()

    test_exe = detect_test_exe(args.bin_dir)
    env = os.environ.copy()
    if args.bin_dir:
        env["PATH"] = str(Path(args.bin_dir).resolve()) + os.pathsep + env.get("PATH", "")
    seq_dir = Path(__file__).resolve().parents[0] / "seq_files"
    if args.all:
        # Full sweeps are useful locally when debugging interaction behavior
        # against many fixtures, but are intentionally not the CI default.
        files = sorted(seq_dir.glob("*.seq"))
    else:
        # Keep this runner focused on representative interaction cases.
        # In practice this Qt GUI test is more reliable as a local check than as
        # a cloud CI gate; broad sequence-open coverage is handled separately by
        # test_load_all.py across all seqs.
        files = [seq_dir / name for name in DEFAULT_CASES]
    missing = [str(f) for f in files if not f.exists()]
    if missing:
        for f in missing:
            print(f"[MISSING zoom/pan] {f}", file=sys.stderr)
        print(
            "[MISSING zoom/pan] One or more representative fixtures are absent in this checkout. "
            "If the file exists locally but not in CI, it is likely untracked or not committed.",
            file=sys.stderr,
        )
        sys.exit(2)
    rc = 0
    for f in files:
        print(f"[TEST zoom/pan] {f}")
        env["TIME_SLIDER_TEST_SEQ"] = str(f.resolve())
        try:
            cp = subprocess.run(
                [test_exe, "-o", "-,txt"],
                text=True,
                env=env,
                capture_output=True,
                timeout=args.timeout,
            )
        except subprocess.TimeoutExpired as exc:
            if exc.stdout:
                print(exc.stdout, end="" if exc.stdout.endswith("\n") else "\n")
            if exc.stderr:
                print(exc.stderr, file=sys.stderr, end="" if exc.stderr.endswith("\n") else "\n")
            print(f"[TIMEOUT zoom/pan] {f} exceeded {args.timeout}s", file=sys.stderr)
            sys.exit(124)
        if cp.stdout:
            print(cp.stdout, end="" if cp.stdout.endswith("\n") else "\n")
        if cp.stderr:
            print(cp.stderr, file=sys.stderr, end="" if cp.stderr.endswith("\n") else "\n")
        if cp.returncode != 0:
            print(f"[FAIL zoom/pan] {f} (exit={cp.returncode})", file=sys.stderr)
            if cp.returncode == 3221225781:
                print("[FAIL zoom/pan] Windows status 0xC0000135: process could not start, likely missing DLL such as Qt6Test.dll.",
                      file=sys.stderr)
            rc = cp.returncode
            break
    sys.exit(rc)

if __name__ == "__main__":
    main()
