"""
Visual Regression Test for SeqEyes using SVG text comparison.

Strategy:
  1. Run seqeyes.exe with --capture-snapshots to produce .svg files.
  2. Compare each snapshot SVG against its baseline SVG using normalised XML text diffing.
     - All floating-point numbers are rounded to 2 decimal places before comparison so that
       tiny rendering jitter does not produce false positives.
     - No native library (Cairo, GTK, Inkscape …) is required – only the Python standard library.
  3. Optionally, if Pillow is available, generate a PNG diff by rasterising with Qt's own SVG
     renderer (not used in CI).  In practice we skip rasterisation and rely on pure text diff.
"""

import subprocess
import os
import sys
import re
import tempfile
import shutil
import argparse
from pathlib import Path


def normalize_svg_text(svg_path: str) -> str:
    """
    Reads an SVG file and normalises it for stable comparison:
      - Rounds all floating-point numbers to 2 decimal places.
      - Strips the <dc:date> metadata element (contains a generation timestamp).
    """
    with open(svg_path, "r", encoding="utf-8") as f:
        content = f.read()

    # Strip timestamp metadata that SVG generators embed
    content = re.sub(r"<dc:date>.*?</dc:date>", "", content, flags=re.DOTALL)

    # Round all floating-point literals to 2 dp so tiny FP jitter doesn't matter
    def _round(m: re.Match) -> str:
        return f"{float(m.group(0)):.2f}"

    content = re.sub(r"-?\d+\.\d+", _round, content)
    return content


def compare_svgs(baseline_path: str, snapshot_path: str, diff_threshold: float = 0.01) -> str:
    """
    Compare two SVG files as normalised text.
    Returns 'PASS', 'FAIL', or 'SKIP'.
    diff_threshold: maximum fraction of differing lines that is still considered a PASS.
    """
    if not os.path.exists(baseline_path):
        print(f"  -> [SKIP] Baseline missing: {os.path.basename(baseline_path)}")
        return "SKIP"

    if not os.path.exists(snapshot_path):
        print(f"  -> [FAIL] Snapshot missing: {os.path.basename(snapshot_path)}")
        return "FAIL"

    baseline_text = normalize_svg_text(baseline_path)
    snapshot_text = normalize_svg_text(snapshot_path)

    if baseline_text == snapshot_text:
        print("  -> [PASS] SVG files match exactly.")
        return "PASS"

    # Count differing lines for a friendlier error message
    b_lines = baseline_text.splitlines()
    s_lines = snapshot_text.splitlines()
    diff_count = sum(1 for a, b in zip(b_lines, s_lines) if a != b)
    diff_count += abs(len(b_lines) - len(s_lines))
    total_lines = max(len(b_lines), len(s_lines), 1)
    diff_pct = diff_count / total_lines

    if diff_pct <= diff_threshold:
        print(f"  -> [PASS] SVG files match within tolerance ({diff_pct*100:.2f}% of lines differ).")
        return "PASS"
    else:
        print(f"  -> [FAIL] SVG files differ ({diff_count}/{total_lines} lines, {diff_pct*100:.2f}%).")
        return "FAIL"


def main():
    parser = argparse.ArgumentParser(description="Run SVG Visual Regression Tests for SeqEyes.")
    parser.add_argument("--seq-dir",      type=str, default="test/seq_files",  help="Directory containing .seq files")
    parser.add_argument("--bin-dir",      type=str, default="build/Release",   help="Directory containing seqeyes.exe")
    parser.add_argument("--out-dir",      type=str, default="test/snapshots",  help="Directory to save generated snapshots")
    parser.add_argument("--baseline-dir", type=str, default="test/baselines",  help="Directory containing golden SVG baselines")
    args = parser.parse_args()

    seq_dir      = Path(args.seq_dir)
    exe_path     = Path(args.bin_dir) / "seqeyes.exe"
    out_dir      = Path(args.out_dir)
    baseline_dir = Path(args.baseline_dir)

    if not exe_path.exists():
        print(f"[ERROR] Executable not found: {exe_path}")
        sys.exit(1)
    if not seq_dir.exists():
        print(f"[ERROR] Seq directory not found: {seq_dir}")
        sys.exit(1)

    out_dir.mkdir(parents=True, exist_ok=True)
    baseline_dir.mkdir(parents=True, exist_ok=True)

    TARGET_SEQS = [
        # "writeEpiRS_label_softdelay",
        # "writeEpiRS_label",
        # "writeEpiSpinEchoRS",
        # "writeFastRadialGradientEcho",
        # "writeFid",
        # "writeGradientEcho_grappa",
        "writeGradientEcho_label",
        # "writeGradientEcho",
        # "writeGRE_live_demo",
        # "writeGRE_live_demo_step0",
        # "writeHASTE",
        # "writeRadialGradientEcho_rotExt",
        # "writeRadialGradientEcho",
        # "writeSemiLaser",
        # "writeSpiral",
        # "writeTrufi",
        # "writeTSE",
        # "writeUTE_rs",
        # "writeUTE",
        # "epi",
        # "spi",
        # "spi_sub",
        # "writeCineGradientEcho"
    ]

    seq_files = []
    for name in TARGET_SEQS:
        fpath = seq_dir / f"{name}.seq"
        if fpath.exists():
            seq_files.append(fpath)
        else:
            print(f"[WARNING] Target sequence not found: {fpath}")

    if not seq_files:
        print(f"[WARNING] No target .seq files found in {seq_dir}")
        sys.exit(0)

    print(f"Found {len(seq_files)} sequence files. Running SVG visual regression tests...\n")

    qt_env = os.environ.copy()
    qt_env["QT_ENABLE_HIGHDPI_SCALING"] = "0"
    qt_env["QT_SCALE_FACTOR"] = "1"
    qt_env["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0"

    # Inject Qt6 bin path on local machines to prevent STATUS_DLL_NOT_FOUND (0xC0000135).
    # On CI the Qt installer action already adds Qt to PATH.
    qt_bin_path = r"C:\Qt\6.5.3\msvc2019_64\bin"
    if os.path.exists(qt_bin_path):
        qt_env["PATH"] = qt_bin_path + os.pathsep + qt_env.get("PATH", "")

    total_passed  = 0
    total_failed  = 0
    total_skipped = 0
    failed_names  = []

    for seq_file in seq_files:
        base_name = seq_file.stem
        print(f"--- [{base_name}] ---")

        # Generate SVG snapshots (both _seq and _traj in a single run)
        with tempfile.TemporaryDirectory() as tmp_dir:
            capture_ok = False
            try:
                res = subprocess.run(
                    [str(exe_path), "--Whole-sequence", "--time-range", "0~10",
                     "--capture-snapshots", tmp_dir, str(seq_file)],
                    capture_output=True, text=True, timeout=120, env=qt_env
                )
                if res.returncode != 0:
                    print(f"  -> [CRASH] seqeyes.exe exited with code {res.returncode}.")
                    if res.stderr:
                        print(f"     Stderr: {res.stderr.strip()[:200]}")

                src_seq  = Path(tmp_dir) / f"{base_name}_seq.svg"
                src_traj = Path(tmp_dir) / f"{base_name}_traj.svg"

                if src_seq.exists():
                    shutil.copy2(src_seq,  out_dir / f"{base_name}_seq.svg")
                if src_traj.exists():
                    shutil.copy2(src_traj, out_dir / f"{base_name}_traj.svg")

                capture_ok = src_seq.exists() and src_traj.exists()

            except subprocess.TimeoutExpired:
                print(f"  -> [FAIL] Timeout (>120s).")

        if not capture_ok:
            print(f"  -> [FAIL] SVG capture failed.")
            total_failed += 1
            failed_names.append(base_name)
            continue

        # Text-based SVG comparison (no native libraries required)
        r_seq  = compare_svgs(
            str(baseline_dir / f"{base_name}_seq.svg"),
            str(out_dir      / f"{base_name}_seq.svg"),
        )
        r_traj = compare_svgs(
            str(baseline_dir / f"{base_name}_traj.svg"),
            str(out_dir      / f"{base_name}_traj.svg"),
        )

        results = [r_seq, r_traj]
        if "FAIL" in results:
            total_failed += 1
            failed_names.append(base_name)
        elif all(r == "SKIP" for r in results):
            total_skipped += 1
        else:
            # At least one PASS, no FAILs → count as passed
            total_passed += 1 if "PASS" in results else 0
            if "SKIP" in results:
                total_skipped += 1

    print(f"\n{'='*44}")
    print(f"    Visual Regression Summary (SVG)     ")
    print(f"{'='*44}")
    print(f"Total Sequences: {len(seq_files)}")
    print(f"Passed:          {total_passed}")
    print(f"Skipped:         {total_skipped}")
    print(f"Failed:          {total_failed}")
    print(f"{'='*44}")

    if total_failed > 0:
        print(f"\n[FAILED] Tests failed for:")
        for name in failed_names:
            print(f"  - {name}")
        sys.exit(1)
    else:
        print(f"\n[SUCCESS] Visual regression tests passed!")


if __name__ == "__main__":
    main()
