import subprocess
import os
import sys
import tempfile
import shutil
import argparse
from pathlib import Path

try:
    from PIL import Image, ImageChops, ImageStat
    HAS_PILLOW = True
except ImportError:
    HAS_PILLOW = False

try:
    import cairosvg
    HAS_CAIROSVG = True
except ImportError:
    HAS_CAIROSVG = False


def svg_to_pil(svg_path: str, width: int = 1000, height: int = 600) -> "Image.Image":
    """Rasterise an SVG to a PIL Image using cairosvg (platform-independent)."""
    png_bytes = cairosvg.svg2png(url=svg_path, output_width=width, output_height=height)
    import io
    return Image.open(io.BytesIO(png_bytes)).convert("RGB")


def compare_svgs(baseline_svg, snapshot_svg, diff_path, threshold=0.005):
    """
    Rasterise both SVGs with cairosvg then compare pixel-by-pixel with PIL.
    Returns 'PASS', 'FAIL', or 'SKIP'.
    """
    if not os.path.exists(baseline_svg):
        print(f"  -> [SKIP] Baseline missing: {os.path.basename(baseline_svg)}")
        return "SKIP"

    if not os.path.exists(snapshot_svg):
        print(f"  -> [FAIL] Snapshot missing: {os.path.basename(snapshot_svg)}")
        return "FAIL"

    img1 = svg_to_pil(baseline_svg)
    img2 = svg_to_pil(snapshot_svg)

    if img1.size != img2.size:
        print(f"  -> [FAIL] Size mismatch after rasterise: {img1.size} vs {img2.size}")
        return "FAIL"

    diff = ImageChops.difference(img1, img2)
    stat = ImageStat.Stat(diff)
    mean_diff = sum(stat.mean) / (len(stat.mean) * 255.0)

    if mean_diff > threshold:
        print(f"  -> [FAIL] Images differ by {mean_diff*100:.3f}% (threshold {threshold*100:.2f}%)")
        diff.save(diff_path)
        print(f"       Diff saved to {diff_path}")
        return "FAIL"

    print(f"  -> [PASS] Images match (diff: {mean_diff*100:.4f}%)")
    return "PASS"


def main():
    parser = argparse.ArgumentParser(description="Run SVG-based Visual Regression Tests for SeqEyes.")
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
        print(f"[ERROR] Executable not found at {exe_path}")
        sys.exit(1)

    if not seq_dir.exists() or not seq_dir.is_dir():
        print(f"[ERROR] Sequence directory not found: {seq_dir}")
        sys.exit(1)

    out_dir.mkdir(parents=True, exist_ok=True)
    baseline_dir.mkdir(parents=True, exist_ok=True)

    if not HAS_PILLOW:
        print("[WARNING] Pillow not installed. Install with: pip install Pillow")
        sys.exit(1)

    if not HAS_CAIROSVG:
        print("[WARNING] cairosvg not installed. Install with: pip install cairosvg")
        sys.exit(1)

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

    # Inject Qt6 bin path into the environment to prevent STATUS_DLL_NOT_FOUND (0xC0000135)
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

        # Generate SVG snapshots for this sequence
        with tempfile.TemporaryDirectory() as tmp_dir:
            capture_ok = False
            try:
                res = subprocess.run(
                    [str(exe_path), "--Whole-sequence", "--time-range", "0~10",
                     "--capture-snapshots", tmp_dir, str(seq_file)],
                    capture_output=True, text=True, timeout=120, env=qt_env
                )
                if res.returncode != 0:
                    print(f"  -> [CRASH] seqeyes.exe exited with code {res.returncode}. Stderr: {res.stderr.strip()[:200]}")

                src_seq  = Path(tmp_dir) / f"{base_name}_seq.svg"
                src_traj = Path(tmp_dir) / f"{base_name}_traj.svg"

                if src_seq.exists():
                    shutil.copy2(src_seq,  out_dir / f"{base_name}_seq.svg")
                if src_traj.exists():
                    shutil.copy2(src_traj, out_dir / f"{base_name}_traj.svg")

                capture_ok = src_seq.exists() and src_traj.exists()

            except subprocess.TimeoutExpired:
                print(f"  -> [FAIL] Timeout (>120s)")

        if not capture_ok:
            print(f"  -> [FAIL] SVG capture failed for {base_name}")
            total_failed += 1
            failed_names.append(base_name)
            continue

        # Compare with baselines
        snap_seq  = str(out_dir      / f"{base_name}_seq.svg")
        snap_traj = str(out_dir      / f"{base_name}_traj.svg")
        base_seq  = str(baseline_dir / f"{base_name}_seq.svg")
        base_traj = str(baseline_dir / f"{base_name}_traj.svg")
        diff_seq  = str(out_dir      / f"{base_name}_seq_diff.png")
        diff_traj = str(out_dir      / f"{base_name}_traj_diff.png")

        r_seq  = compare_svgs(base_seq,  snap_seq,  diff_seq)
        r_traj = compare_svgs(base_traj, snap_traj, diff_traj)

        results = [r_seq, r_traj]
        if "FAIL" in results:
            total_failed += 1
            failed_names.append(base_name)
        elif all(r == "SKIP" for r in results):
            total_skipped += 1
        elif "SKIP" in results:
            total_skipped += 1  # partial skip counts as skip
        else:
            total_passed += 1

    print(f"\n{'='*42}")
    print(f"      Visual Regression Summary (SVG)    ")
    print(f"{'='*42}")
    print(f"Total Sequences: {len(seq_files)}")
    print(f"Passed Checks:   {total_passed}")
    print(f"Skipped Checks:  {total_skipped}")
    print(f"Failed Checks:   {total_failed}")
    print(f"{'='*42}")

    if total_failed > 0:
        print(f"\n[FAILED] Visual regression tests failed for:")
        for name in failed_names:
            print(f"  - {name}")
        print(f"\nCheck the diff images in {out_dir}/")
        sys.exit(1)
    else:
        print(f"\n[SUCCESS] Visual regression tests passed!")

if __name__ == "__main__":
    main()
