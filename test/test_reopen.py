import argparse
import os
import subprocess
import sys
import threading
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SEQ_DIR = REPO / "test" / "seq_files"
QT_BIN_DEFAULT = Path(r"C:\Qt\6.5.3\msvc2019_64\bin")


def _pump(stream, sink):
    try:
        for line in iter(stream.readline, ""):
            sink.write(line)
            sink.flush()
    finally:
        stream.close()


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


def detect_platform_plugin(bin_dir: Path, qt_bin: Path) -> tuple[str, Path | None, list[Path]]:
    platform_dirs = [
        bin_dir / "platforms",
        qt_bin / "platforms",
        qt_bin.parent / "plugins" / "platforms",
    ]
    plugins: list[Path] = []
    seen: set[str] = set()
    for d in platform_dirs:
        if d.exists():
            for plugin in sorted(d.glob("q*.dll")):
                key = str(plugin.resolve()).lower()
                if key not in seen:
                    seen.add(key)
                    plugins.append(plugin)

    by_name = {p.name.lower(): p for p in plugins}
    if "qoffscreen.dll" in by_name:
        return "offscreen", by_name["qoffscreen.dll"], plugins
    if "qminimal.dll" in by_name:
        return "minimal", by_name["qminimal.dll"], plugins
    if "qwindows.dll" in by_name:
        return "windows", by_name["qwindows.dll"], plugins
    return "windows", None, plugins


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
    # Prefer minimal when it is actually deployed, but windeployqt usually ships
    # qwindows.dll only. Forcing a missing platform plugin can block in
    # QApplication startup on hosted Windows before QtTest prints anything.
    platform, platform_plugin, platform_plugins = detect_platform_plugin(args.bin_dir, args.qt_bin)
    env.setdefault("QT_QPA_PLATFORM", platform)
    if platform_plugin is not None:
        env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(platform_plugin.parent.resolve())
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

    print(f"[TEST reopen] exe={exe}", flush=True)
    print(f"[TEST reopen] file_a={env['REOPEN_SEQ_A']}", flush=True)
    print(f"[TEST reopen] file_b={env['REOPEN_SEQ_B']}", flush=True)
    print(f"[TEST reopen] QT_QPA_PLATFORM={env.get('QT_QPA_PLATFORM', '')}", flush=True)
    print(f"[TEST reopen] QT_QPA_PLATFORM_PLUGIN_PATH={env.get('QT_QPA_PLATFORM_PLUGIN_PATH', '')}", flush=True)
    if platform_plugins:
        print("[TEST reopen] platform_plugins=" + ";".join(str(p) for p in platform_plugins), flush=True)
    else:
        print("[TEST reopen] platform_plugins=<none>", flush=True)
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
