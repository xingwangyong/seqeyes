#!/usr/bin/env python3
import subprocess
from pathlib import Path
import sys
import argparse
import os

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
    args = ap.parse_args()

    test_exe = detect_test_exe(args.bin_dir)
    seq_dir = Path(__file__).resolve().parents[0] / "seq_files"
    files = sorted(seq_dir.glob("*.seq"))
    rc = 0
    for f in files:
        print(f"[TEST zoom/pan] {f}")
        cp = subprocess.run([test_exe, "--file", str(f)], text=True)
        if cp.returncode != 0:
            rc = cp.returncode
    sys.exit(rc)

if __name__ == "__main__":
    main()
