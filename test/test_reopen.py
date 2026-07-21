import argparse
import subprocess
import sys
import threading
from pathlib import Path

from qt_test_utils import REPO, default_qt_bin, detect_exe, print_qt_test_header, qt_test_env


SEQ_DIR = REPO / "test" / "seq_files"


def _pump(stream, sink):
    try:
        for line in iter(stream.readline, ""):
            sink.write(line)
            sink.flush()
    finally:
        stream.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin-dir", type=Path, default=REPO / "out" / "build" / "x64-Release")
    ap.add_argument("--qt-bin", type=Path, default=default_qt_bin(),
                    help="Qt bin dir; put on PATH so the test finds Qt DLLs/plugins")
    ap.add_argument("--file-a", type=Path, default=SEQ_DIR / "writeGradientEcho_label.seq",
                    help="First sequence (the one opened earlier)")
    ap.add_argument("--file-b", type=Path, default=SEQ_DIR / "writeFid.seq",
                    help="Second sequence (must not inherit A's state)")
    ap.add_argument("--timeout", type=int, default=360,
                    help="Whole-test timeout in seconds")
    args = ap.parse_args()

    exe = detect_exe(args.bin_dir, "ReopenEquivalenceTest")
    env, _platform, platform_plugins = qt_test_env(exe, args.bin_dir, args.qt_bin)
    env["REOPEN_SEQ_A"] = str(args.file_a.resolve())
    env["REOPEN_SEQ_B"] = str(args.file_b.resolve())
    env["REOPEN_TEST_VERBOSE"] = "1"
    env.setdefault("REOPEN_TEST_INTERNAL_TIMEOUT_MS", str(max(args.timeout - 5, 1) * 1000))

    print_qt_test_header("reopen", exe, env, platform_plugins)
    print(f"[TEST reopen] file_a={env['REOPEN_SEQ_A']}", flush=True)
    print(f"[TEST reopen] file_b={env['REOPEN_SEQ_B']}", flush=True)

    proc = subprocess.Popen(
        [str(exe), "-o", "-,txt", "-v1"],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    t_out = threading.Thread(target=_pump, args=(proc.stdout, sys.stdout), daemon=True)
    t_err = threading.Thread(target=_pump, args=(proc.stderr, sys.stderr), daemon=True)
    t_out.start()
    t_err.start()
    try:
        rc = proc.wait(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        t_out.join(timeout=5)
        t_err.join(timeout=5)
        print(f"[TIMEOUT reopen] exceeded {args.timeout}s", file=sys.stderr)
        sys.exit(124)
    t_out.join(timeout=5)
    t_err.join(timeout=5)
    if rc != 0:
        print(f"[FAIL reopen] exit={rc}", file=sys.stderr)
    sys.exit(rc)


if __name__ == "__main__":
    main()
