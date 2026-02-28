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
    
    if mean_diff > threshold:
        print(f"  -> [FAIL] Images differ by {mean_diff*100:.2f}% (Threshold: {threshold*100:.2f}%)")
        diff.save(diff_path)
        print(f"       Saved diff to {diff_path}")
        return "FAIL"
        
    print(f"  -> [PASS] Images match (Difference: {mean_diff*100:.4f}%)")
    return "PASS"

def main():
    parser = argparse.ArgumentParser(description="Run Visual Regression Tests for SeqEyes.")
    parser.add_argument("--seq-dir", type=str, default="test/seq_files", help="Directory containing .seq files")
    parser.add_argument("--bin-dir", type=str, default="build/Release", help="Directory containing seqeyes.exe")
    parser.add_argument("--out-dir", type=str, default="test/snapshots", help="Directory to save the generated snapshots")
    parser.add_argument("--baseline-dir", type=str, default="test/baselines", help="Directory containing golden baselines")
    
    args = parser.parse_args()
    
    seq_dir = Path(args.seq_dir)
    exe_path = Path(args.bin_dir) / "seqeyes.exe"
    out_dir = Path(args.out_dir)
    baseline_dir = Path(args.baseline_dir)
    
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
        
    TARGET_SEQS = [
        "writeEpiRS_label_softdelay",
        "writeEpiRS_label",
        "writeEpiSpinEchoRS",
        "writeFastRadialGradientEcho",
        "writeFid",
        "writeGradientEcho_grappa",
        "writeGradientEcho_label",
        "writeGradientEcho",
        "writeGRE_live_demo",
        "writeGRE_live_demo_step0",
        "writeHASTE",
        "writeRadialGradientEcho_rotExt",
        "writeRadialGradientEcho",
        "writeSemiLaser",
        "writeSpiral",
        "writeTrufi",
        "writeTSE",
        "writeUTE_rs",
        "writeUTE",
        "epi",
        "spi",
        "spi_sub",
        "writeCineGradientEcho"
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
        
    print(f"Found {len(seq_files)} sequence files from the target list. Running visual regression tests...")
    
    qt_env = os.environ.copy()
    qt_env["QT_ENABLE_HIGHDPI_SCALING"] = "0"
    qt_env["QT_SCALE_FACTOR"] = "1"
    qt_env["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0"
    
    total_passed = 0
    total_failed = 0
    total_skipped = 0
    failed_seq_names = []
    
    for seq_file in seq_files:
        base_name = seq_file.stem
        print(f"\n--- [{base_name}] ---")
        
        # Run 1: Target Sequence Diagram (0-10ms)
        seq_success = False
        with tempfile.TemporaryDirectory() as tmp_seq:
            try:
                subprocess.run(
                    [str(exe_path), "--Whole-sequence", "--time-range", "0~10", "--capture-snapshots", tmp_seq, str(seq_file)],
                    capture_output=True, text=True, timeout=60, env=qt_env
                )
                src_seq = Path(tmp_seq) / f"{base_name}_seq.png"
                if src_seq.exists():
                    shutil.copy2(src_seq, out_dir / f"{base_name}_seq.png")
                    seq_success = True
                else:
                    print(f"  -> [FAIL] Failed to capture sequence diagram.")
            except subprocess.TimeoutExpired:
                print(f"  -> [FAIL] Timeout during SEQ capture (>60s).")
                
        # Run 2: Target Trajectory Diagram (Whole sequence)
        traj_success = False
        with tempfile.TemporaryDirectory() as tmp_traj:
            try:
                subprocess.run(
                    [str(exe_path), "--Whole-sequence", "--capture-snapshots", tmp_traj, str(seq_file)],
                    capture_output=True, text=True, timeout=60, env=qt_env
                )
                src_traj = Path(tmp_traj) / f"{base_name}_traj.png"
                if src_traj.exists():
                    shutil.copy2(src_traj, out_dir / f"{base_name}_traj.png")
                    traj_success = True
                else:
                    print(f"  -> [FAIL] Failed to capture trajectory diagram.")
            except subprocess.TimeoutExpired:
                print(f"  -> [FAIL] Timeout during TRAJ capture (>60s).")
                
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
                res = compare_images(str(base_path), str(snap_path), str(diff_path))
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
    print(f"Total Sequences: {len(seq_files)}")
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
