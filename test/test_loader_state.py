import argparse
import os
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
QT_BIN_DEFAULT = Path(r"C:\Qt\6.5.3\msvc2019_64\bin")


def detect_exe(bin_dir: Path) -> Path:
    for candidate in [
        bin_dir / "PulseqLoaderStateTest.exe",
        bin_dir / "test" / "PulseqLoaderStateTest.exe",
        bin_dir / "test" / "Release" / "PulseqLoaderStateTest.exe",
        bin_dir / "test" / "Debug" / "PulseqLoaderStateTest.exe",
        bin_dir.parent / "test" / "Release" / "PulseqLoaderStateTest.exe",
        bin_dir.parent / "test" / "Debug" / "PulseqLoaderStateTest.exe",
        bin_dir / "PulseqLoaderStateTest",
        bin_dir / "test" / "PulseqLoaderStateTest",
    ]:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f"PulseqLoaderStateTest not found under {bin_dir}")


def detect_platform_plugin(bin_dir: Path, qt_bin: Path) -> tuple[str, Path | None, list[Path]]:
    platform_dirs = [
        bin_dir / "platforms",
        qt_bin / "platforms",
        qt_bin.parent / "plugins" / "platforms",
    ]
    plugins: list[Path] = []
    seen: set[str] = set()
    for directory in platform_dirs:
        if not directory.exists():
            continue
        for plugin in sorted(directory.glob("q*.dll")):
            key = str(plugin.resolve()).lower()
            if key not in seen:
                seen.add(key)
                plugins.append(plugin)

    by_name = {plugin.name.lower(): plugin for plugin in plugins}
    if "qoffscreen.dll" in by_name:
        return "offscreen", by_name["qoffscreen.dll"], plugins
    if "qminimal.dll" in by_name:
        return "minimal", by_name["qminimal.dll"], plugins
    if "qwindows.dll" in by_name:
        return "windows", by_name["qwindows.dll"], plugins
    return "windows", None, plugins


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", type=Path, default=REPO / "out" / "build" / "x64-Release")
    parser.add_argument(
        "--qt-bin",
        type=Path,
        default=QT_BIN_DEFAULT,
        help="Qt bin dir; added to PATH so the test finds Qt DLLs/plugins",
    )
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()

    exe = detect_exe(args.bin_dir)
    exe_dir = exe.parent.resolve()
    platform, platform_plugin, platform_plugins = detect_platform_plugin(exe_dir, args.qt_bin)

    env = os.environ.copy()
    env["QT_ENABLE_HIGHDPI_SCALING"] = "0"
    env["QT_SCALE_FACTOR"] = "1"
    env["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0"
    env.setdefault("QT_QPA_PLATFORM", platform)
    if platform_plugin is not None:
        env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(platform_plugin.parent.resolve())

    path_entries = [str(exe_dir)]
    if args.bin_dir.exists():
        path_entries.append(str(args.bin_dir.resolve()))
    if args.qt_bin.exists():
        path_entries.append(str(args.qt_bin.resolve()))
    env["PATH"] = os.pathsep.join(path_entries + [env.get("PATH", "")])

    print(f"[TEST loader-state] exe={exe}", flush=True)
    print(f"[TEST loader-state] QT_QPA_PLATFORM={env.get('QT_QPA_PLATFORM', '')}", flush=True)
    print(
        f"[TEST loader-state] QT_QPA_PLATFORM_PLUGIN_PATH={env.get('QT_QPA_PLATFORM_PLUGIN_PATH', '')}",
        flush=True,
    )
    if platform_plugins:
        print("[TEST loader-state] platform_plugins=" + ";".join(str(p) for p in platform_plugins), flush=True)
    else:
        print("[TEST loader-state] platform_plugins=<none>", flush=True)

    try:
        completed = subprocess.run(
            [str(exe), "-o", "-,txt", "-v1"],
            env=env,
            text=True,
            timeout=args.timeout,
        )
    except subprocess.TimeoutExpired:
        print(f"[TIMEOUT loader-state] exceeded {args.timeout}s", file=sys.stderr)
        return 124

    if completed.returncode == 3221225781:
        print(
            "[FAIL loader-state] Windows status 0xC0000135: process could not start, likely missing a DLL.",
            file=sys.stderr,
        )
    elif completed.returncode != 0:
        print(f"[FAIL loader-state] exit={completed.returncode}", file=sys.stderr)
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main())
