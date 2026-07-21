import argparse
import sys
from pathlib import Path

from qt_test_utils import REPO, default_qt_bin, detect_exe, print_qt_test_header, qt_test_env, run_qt_test


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", type=Path, default=REPO / "out" / "build" / "x64-Release")
    parser.add_argument(
        "--qt-bin",
        type=Path,
        default=default_qt_bin(),
        help="Qt bin dir; added to PATH so the test finds Qt DLLs/plugins",
    )
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()

    exe = detect_exe(args.bin_dir, "PulseqLoaderStateTest")
    env, _platform, platform_plugins = qt_test_env(exe, args.bin_dir, args.qt_bin)
    print_qt_test_header("loader-state", exe, env, platform_plugins)
    return run_qt_test("loader-state", exe, env, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
