import subprocess
import os
import sys
import tempfile
import shutil
import argparse
from pathlib import Path
try:
    import yaml
except ImportError:
    yaml = None

try:
    from PIL import Image, ImageChops, ImageStat
    HAS_PILLOW = True
except ImportError:
    HAS_PILLOW = False


def load_visual_targets(config_path: Path):
    # Shared visual targets format:
    # - Read only "targets" list.
    # - Each item has only two fields:
    #   "seqname" and "seq_diagram_time_range_ms".
    if yaml is None:
        print("[ERROR] Missing dependency: pyyaml. Install it in CI before running this script.")
        sys.exit(2)

    if not config_path.exists():
        print(f"[ERROR] Targets config not found: {config_path}")
        sys.exit(1)

    try:
        with open(config_path, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f) or {}
    except Exception as e:
        print(f"[ERROR] Failed to parse YAML config {config_path}: {e}")
        sys.exit(1)

    targets = data.get("targets", [])
    if not isinstance(targets, list):
        print(f"[ERROR] Invalid format in {config_path}: 'targets' must be a list")
        sys.exit(1)

    parsed = []
    for i, t in enumerate(targets, start=1):
        if not isinstance(t, dict):
            print(f"[ERROR] Invalid target at index {i}: expected mapping")
            sys.exit(1)
        seqname = str(t.get("seqname", "")).strip()
        seq_range = str(t.get("seq_diagram_time_range_ms", "")).strip()
        if not seqname or not seq_range:
            print(f"[ERROR] Invalid target at index {i}: seqname and seq_diagram_time_range_ms are required")
            sys.exit(1)
        parsed.append({"seqname": seqname, "seq_diagram_time_range_ms": seq_range})

    return parsed

def compare_images(baseline_path, snapshot_path, diff_path, threshold=0.005):
    """
    Compares two images using Pillow. 
    Returns 'PASS', 'FAIL', or 'SKIP'.
    Saves a diff image if they differ.
    """
    if not os.path.exists(baseline_path):
        print(f"  -> [SKIP] Baseline missing: {os.path.basename(baseline_path)}")
        return "SKIP"

    img1 = Image.open(baseline_path).convert('RGB')
    img2 = Image.open(snapshot_path).convert('RGB')
    
    if img1.size != img2.size:
        print(f"  -> [FAIL] Size mismatch: Baseline {img1.size} vs Snapshot {img2.size}. (DPI Scaling Issue?)")
        return "FAIL"
        
    diff = ImageChops.difference(img1, img2)
    stat = ImageStat.Stat(diff)
    
    # Calculate mean difference across all channels as a percentage
    mean_diff = sum(stat.mean) / (len(stat.mean) * 255.0)
    # Ratio of changed pixels (at least one channel differs)
    gray = diff.convert("L")
    hist = gray.histogram()
    total_pixels = img1.size[0] * img1.size[1]
    changed_pixels = total_pixels - hist[0]
    changed_ratio = changed_pixels / total_pixels if total_pixels > 0 else 0.0
    
    if mean_diff > threshold:
        print(
            f"  -> [FAIL] Mean diff {mean_diff*100:.2f}% (Threshold: {threshold*100:.2f}%), "
            f"changed pixels {changed_ratio*100:.2f}%"
        )
        diff.save(diff_path)
        print(f"       Saved diff to {diff_path}")
        return "FAIL"
        
    print(
        f"  -> [PASS] Images match (Mean diff: {mean_diff*100:.4f}%, "
        f"changed pixels: {changed_ratio*100:.4f}%)"
    )
    return "PASS"

def main():
    parser = argparse.ArgumentParser(description="Run Visual Regression Tests for SeqEyes.")
    parser.add_argument("--seq-dir", type=str, default="test/seq_files", help="Directory containing .seq files")
    parser.add_argument("--bin-dir", type=str, default="build/Release", help="Directory containing seqeyes.exe")
    parser.add_argument("--out-dir", type=str, default="test/snapshots", help="Directory to save the generated snapshots")
    parser.add_argument("--baseline-dir", type=str, default="test/baselines", help="Directory containing golden baselines")
    parser.add_argument("--threshold", type=float, default=0.005, help="Mean pixel diff threshold (default: 0.005 => 0.5%)")
    parser.add_argument("--targets-config", type=str, default="test/visual_targets.yaml", help="YAML file containing visual regression targets")
    
    args = parser.parse_args()
    
    seq_dir = Path(args.seq_dir)
    exe_path = Path(args.bin_dir) / "seqeyes.exe"
    out_dir = Path(args.out_dir)
    baseline_dir = Path(args.baseline_dir)
    targets_config = Path(args.targets_config)
    
    # Ensure binary exists
    if not exe_path.exists():
        print(f"[ERROR] Executable not found at {exe_path}")
        sys.exit(1)
        
    if not seq_dir.exists() or not seq_dir.is_dir():
        print(f"[ERROR] Sequence directory not found: {seq_dir}")
        sys.exit(1)
        
    out_dir.mkdir(parents=True, exist_ok=True)
    baseline_dir.mkdir(parents=True, exist_ok=True)
    
    if not HAS_PILLOW:
        print("\n[WARNING] Pillow is not installed. Will skip image comparison.")

    targets = load_visual_targets(targets_config)

    seq_items = []
    for t in targets:
        name = t["seqname"]
        fpath = seq_dir / f"{name}.seq"
        if fpath.exists():
            seq_items.append((fpath, t["seq_diagram_time_range_ms"]))
        else:
            print(f"[WARNING] Target sequence not found: {fpath}")
            
    if not seq_items:
        print(f"[WARNING] No target .seq files found in {seq_dir}")
        sys.exit(0)
        
    print(f"Found {len(seq_items)} sequence files from targets config. Running visual regression tests...")
    
    qt_env = os.environ.copy()
    qt_env["QT_ENABLE_HIGHDPI_SCALING"] = "0"
    qt_env["QT_SCALE_FACTOR"] = "1"
    qt_env["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0"
    
    # Inject Qt6 bin path into the environment to prevent STATUS_DLL_NOT_FOUND (0xC0000135)
    qt_bin_path = r"C:\Qt\6.5.3\msvc2019_64\bin"
    if os.path.exists(qt_bin_path):
        qt_env["PATH"] = qt_bin_path + os.pathsep + qt_env.get("PATH", "")
    
    total_passed = 0
    total_failed = 0
    total_skipped = 0
    failed_seq_names = []
    
    for seq_file, seq_range in seq_items:
        base_name = seq_file.stem
        print(f"\n--- [{base_name}] --- (seq range: {seq_range})")
        
        # Sequence diagram uses seq_diagram_time_range_ms from YAML.
        seq_success = False
        with tempfile.TemporaryDirectory() as tmp_seq:
            try:
                subprocess.run(
                    [str(exe_path), "--Whole-sequence", "--time-range", seq_range, "--capture-snapshots", tmp_seq, str(seq_file)],
                    capture_output=True, text=True, timeout=120, env=qt_env
                )
                src_seq = Path(tmp_seq) / f"{base_name}_seq.png"
                if src_seq.exists():
                    shutil.copy2(src_seq, out_dir / f"{base_name}_seq.png")
                    seq_success = True
                else:
                    print(f"  -> [FAIL] Failed to capture sequence diagram.")
            except subprocess.TimeoutExpired:
                print(f"  -> [FAIL] Timeout during SEQ capture (>120s).")
                
        # Trajectory diagram is always captured in whole-sequence mode.
        traj_success = False
        with tempfile.TemporaryDirectory() as tmp_traj:
            try:
                subprocess.run(
                    [str(exe_path), "--Whole-sequence", "--capture-snapshots", tmp_traj, str(seq_file)],
                    capture_output=True, text=True, timeout=120, env=qt_env
                )
                src_traj = Path(tmp_traj) / f"{base_name}_traj.png"
                if src_traj.exists():
                    shutil.copy2(src_traj, out_dir / f"{base_name}_traj.png")
                    traj_success = True
                else:
                    print(f"  -> [FAIL] Failed to capture trajectory diagram.")
            except subprocess.TimeoutExpired:
                print(f"  -> [FAIL] Timeout during TRAJ capture (>120s).")
                
        if not (seq_success and traj_success):
            total_failed += 1
            failed_seq_names.append(base_name)
            continue
            
        # 3. Perform Visual Regression Comparison
        if not HAS_PILLOW:
            total_skipped += 1
            print("  -> [SKIP] Pillow missing, cannot compare.")
            continue
            
        has_fail = False
        has_skip = False
        
        for suffix in ["_seq", "_traj"]:
            filename = f"{base_name}{suffix}.png"
            snap_path = out_dir / filename
            base_path = baseline_dir / filename
            diff_path = out_dir / f"{base_name}{suffix}_diff.png"
            
            if snap_path.exists():
                res = compare_images(str(base_path), str(snap_path), str(diff_path), threshold=args.threshold)
                if res == "FAIL":
                    has_fail = True
                elif res == "SKIP":
                    has_skip = True
                    
        if has_fail:
            total_failed += 1
            failed_seq_names.append(base_name)
        elif has_skip:
            total_skipped += 1 # Only counts as skipped if it didn't fail anywhere else
        else:
            total_passed += 1
            
    print("\n==========================================")
    print("      Visual Regression Summary           ")
    print("==========================================")
    print(f"Total Sequences: {len(seq_items)}")
    print(f"Passed Checks:   {total_passed}")
    print(f"Skipped Checks:  {total_skipped}")
    print(f"Failed Checks:   {total_failed}")
    print("==========================================\n")
    
    if total_failed > 0:
        print("[FAILED] Visual regression tests failed for the following sequences:")
        for name in failed_seq_names:
            print(f"  - {name}")
        print("\nCheck the diff images in test/snapshots/")
        sys.exit(1)
    else:
        print("[SUCCESS] Visual regression tests passed!")
        sys.exit(0)

if __name__ == "__main__":
    main()
