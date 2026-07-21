import os
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
QT_BIN_FALLBACK = Path(r"C:\Qt\6.5.3\msvc2019_64\bin")
WINDOWS_DLL_NOT_FOUND = 3221225781


def default_qt_bin() -> Path:
    for key in ("QT_BIN", "QT_BIN_DIR"):
        value = os.environ.get(key)
        if value:
            return Path(value)

    qt_dir = os.environ.get("QT_DIR") or os.environ.get("QTDIR")
    if qt_dir:
        path = Path(qt_dir)
        return path if path.name.lower() == "bin" else path / "bin"

    return QT_BIN_FALLBACK


def detect_exe(bin_dir: Path, exe_name: str) -> Path:
    exe_suffix = ".exe" if os.name == "nt" else ""
    executable = f"{exe_name}{exe_suffix}"
    candidates = [
        bin_dir / executable,
        bin_dir / "test" / executable,
        bin_dir / "test" / "Release" / executable,
        bin_dir / "test" / "Debug" / executable,
        bin_dir.parent / "test" / "Release" / executable,
        bin_dir.parent / "test" / "Debug" / executable,
        bin_dir / exe_name,
        bin_dir / "test" / exe_name,
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f"{exe_name} not found under {bin_dir}")


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


def qt_test_env(exe: Path, bin_dir: Path, qt_bin: Path) -> tuple[dict[str, str], str, list[Path]]:
    exe_dir = exe.parent.resolve()
    platform, platform_plugin, platform_plugins = detect_platform_plugin(exe_dir, qt_bin)

    env = os.environ.copy()
    env["QT_ENABLE_HIGHDPI_SCALING"] = "0"
    env["QT_SCALE_FACTOR"] = "1"
    env["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0"
    env.setdefault("QT_QPA_PLATFORM", platform)
    if platform_plugin is not None:
        env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(platform_plugin.parent.resolve())

    path_entries = [str(exe_dir)]
    if bin_dir.exists():
        path_entries.append(str(bin_dir.resolve()))
    if qt_bin.exists():
        path_entries.append(str(qt_bin.resolve()))
    env["PATH"] = os.pathsep.join(path_entries + [env.get("PATH", "")])
    return env, platform, platform_plugins


def print_qt_test_header(label: str, exe: Path, env: dict[str, str], platform_plugins: list[Path]) -> None:
    print(f"[TEST {label}] exe={exe}", flush=True)
    print(f"[TEST {label}] QT_QPA_PLATFORM={env.get('QT_QPA_PLATFORM', '')}", flush=True)
    print(
        f"[TEST {label}] QT_QPA_PLATFORM_PLUGIN_PATH={env.get('QT_QPA_PLATFORM_PLUGIN_PATH', '')}",
        flush=True,
    )
    if platform_plugins:
        print(f"[TEST {label}] platform_plugins=" + ";".join(str(p) for p in platform_plugins), flush=True)
    else:
        print(f"[TEST {label}] platform_plugins=<none>", flush=True)


def run_qt_test(label: str, exe: Path, env: dict[str, str], timeout: int) -> int:
    try:
        completed = subprocess.run(
            [str(exe), "-o", "-,txt", "-v1"],
            env=env,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        print(f"[TIMEOUT {label}] exceeded {timeout}s", file=sys.stderr)
        return 124

    if completed.returncode == WINDOWS_DLL_NOT_FOUND:
        print(
            f"[FAIL {label}] Windows status 0xC0000135: process could not start, likely missing a DLL.",
            file=sys.stderr,
        )
    elif completed.returncode != 0:
        print(f"[FAIL {label}] exit={completed.returncode}", file=sys.stderr)
    return completed.returncode
