import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SEQ_DIR = REPO / "test" / "seq_files"
QT_BIN_DEFAULT = Path(r"C:\Qt\6.5.3\msvc2019_64\bin")


def detect_exe(bin_dir: Path) -> Path:
    for c in [
        bin_dir / "ReopenEquivalenceTest.exe",
        bin_dir / "test" / "ReopenEquivalenceTest.exe",
        bin_dir / "test" / "Release" / "ReopenEquivalenceTest.exe",
        bin_dir.parent / "test" / "Release" / "ReopenEquivalenceTest.exe",
        bin_dir.parent / "test" / "Debug" / "ReopenEquivalenceTest.exe",
        bin_dir / "ReopenEquivalenceTest",
        bin_dir / "test" / "ReopenEquivalenceTest",
    ]:
        if c.exists():
            return c
    raise FileNotFoundError(f"ReopenEquivalenceTest not found under {bin_dir}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin-dir", type=Path, default=REPO / "out" / "build" / "x64-Release")
    ap.add_argument("--qt-bin", type=Path, default=QT_BIN_DEFAULT,
                    help="Qt bin dir; put on PATH so the test finds Qt DLLs/plugins")
    ap.add_argument("--file-a", type=Path, default=SEQ_DIR / "writeGradientEcho_label.seq",
                    help="First sequence (the one opened earlier)")
    ap.add_argument("--file-b", type=Path, default=SEQ_DIR / "writeFid.seq",
                    help="Second sequence (must not inherit A's state)")
    ap.add_argument("--timeout", type=int, default=240,
                    help="Whole-test timeout in seconds")
    args = ap.parse_args()

    exe = detect_exe(args.bin_dir)

    # Same setup as test_visual_regression.py: native window platform, fixed DPI
    # for determinism, and Qt bin on PATH so the test finds Qt DLLs. The seq files
    # go through env vars so changing files never needs a recompile.
    env = os.environ.copy()
    env["QT_ENABLE_HIGHDPI_SCALING"] = "0"
    env["QT_SCALE_FACTOR"] = "1"
    env["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0"
    path_entries = []
    if args.bin_dir.exists():
        path_entries.append(str(args.bin_dir.resolve()))
    if args.qt_bin.exists():
        path_entries.append(str(args.qt_bin.resolve()))
    if path_entries:
        env["PATH"] = os.pathsep.join(path_entries + [env.get("PATH", "")])
    env["REOPEN_SEQ_A"] = str(args.file_a.resolve())
    env["REOPEN_SEQ_B"] = str(args.file_b.resolve())
    env["REOPEN_TEST_VERBOSE"] = "1"

    print(f"[TEST reopen] exe={exe}")
    print(f"[TEST reopen] file_a={env['REOPEN_SEQ_A']}")
    print(f"[TEST reopen] file_b={env['REOPEN_SEQ_B']}")
    try:
        cp = subprocess.run(
            [str(exe), "-o", "-,txt", "-v1"],
            env=env,
            text=True,
            capture_output=True,
            timeout=args.timeout,
        )
    except subprocess.TimeoutExpired as exc:
        if exc.stdout:
            print(exc.stdout, end="" if exc.stdout.endswith("\n") else "\n")
        if exc.stderr:
            print(exc.stderr, file=sys.stderr, end="" if exc.stderr.endswith("\n") else "\n")
        print(f"[TIMEOUT reopen] exceeded {args.timeout}s", file=sys.stderr)
        sys.exit(124)

    if cp.stdout:
        print(cp.stdout, end="" if cp.stdout.endswith("\n") else "\n")
    if cp.stderr:
        print(cp.stderr, file=sys.stderr, end="" if cp.stderr.endswith("\n") else "\n")
    if cp.returncode != 0:
        print(f"[FAIL reopen] exit={cp.returncode}", file=sys.stderr)
    sys.exit(cp.returncode)


if __name__ == "__main__":
    main()
