import os
import sys
import subprocess
import argparse
import tempfile
import shutil
from pathlib import Path

try:
    import yaml
except ImportError:
    yaml = None


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

def main():
    parser = argparse.ArgumentParser(description="Batch generate visual regression baselines for SeqEyes.")
    parser.add_argument("--seq-dir", type=str, required=True, help="Directory containing .seq files")
    parser.add_argument("--bin-dir", type=str, required=True, help="Directory containing seqeyes.exe")
    parser.add_argument("--out-dir", type=str, required=True, help="Directory to save the generated baselines")
    parser.add_argument("--targets-config", type=str, default="test/visual_targets.yaml", help="YAML file containing visual regression targets")
    
    args = parser.parse_args()
    
    seq_dir = Path(args.seq_dir)
    bin_dir = Path(args.bin_dir)
    out_dir = Path(args.out_dir)
    targets_config = Path(args.targets_config)
    
    exe_path = bin_dir / "seqeyes.exe"
    if not exe_path.exists():
        print(f"[ERROR] Executable not found at {exe_path}")
        sys.exit(1)
        
    if not seq_dir.exists() or not seq_dir.is_dir():
        print(f"[ERROR] Sequence directory not found: {seq_dir}")
        sys.exit(1)
        
    out_dir.mkdir(parents=True, exist_ok=True)

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
        
    print(f"Found {len(seq_items)} sequence files from targets config. Generating baselines...")
    
    success_count = 0
    fail_count = 0
    
    qt_env = os.environ.copy()
    qt_env["QT_ENABLE_HIGHDPI_SCALING"] = "0"
    qt_env["QT_SCALE_FACTOR"] = "1"
    qt_env["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0"
    
    # Inject Qt6 bin path into the environment to prevent STATUS_DLL_NOT_FOUND (0xC0000135)
    qt_bin_path = r"C:\Qt\6.5.3\msvc2019_64\bin"
    if os.path.exists(qt_bin_path):
        qt_env["PATH"] = qt_bin_path + os.pathsep + qt_env.get("PATH", "")
    
    for seq_file, seq_range in seq_items:
        base_name = seq_file.stem
        print(f"\n[{base_name}] Processing... (seq range: {seq_range})")
        
        # Sequence diagram uses seq_diagram_time_range_ms from YAML.
        seq_success = False
        with tempfile.TemporaryDirectory() as tmp_seq:
            try:
                res = subprocess.run(
                    [str(exe_path), "--Whole-sequence", "--time-range", seq_range, "--capture-snapshots", tmp_seq, str(seq_file)],
                    capture_output=True, text=True, timeout=60, env=qt_env
                )
                if res.returncode != 0:
                    print(f"  -> [CRASH] seqeyes.exe exited with code {res.returncode}. Stderr: {res.stderr.strip()}")
                elif res.stderr:
                    print(f"  -> [STDERR/STDOUT] {res.stderr.strip()}")
                src_seq = Path(tmp_seq) / f"{base_name}_seq.png"
                dst_seq = out_dir / f"{base_name}_seq.png"
                if src_seq.exists():
                    shutil.copy2(src_seq, dst_seq)
                    seq_success = True
                else:
                    print(f"  -> [FAIL] Sequence snapshot failed for {base_name}")
            except subprocess.TimeoutExpired:
                print(f"  -> [FAIL] Sequence capture timeout for {base_name}")
                
        # Trajectory diagram is always captured in whole-sequence mode.
        traj_success = False
        with tempfile.TemporaryDirectory() as tmp_traj:
            try:
                subprocess.run(
                    [str(exe_path), "--Whole-sequence", "--capture-snapshots", tmp_traj, str(seq_file)],
                    capture_output=True, text=True, timeout=60, env=qt_env
                )
                src_traj = Path(tmp_traj) / f"{base_name}_traj.png"
                dst_traj = out_dir / f"{base_name}_traj.png"
                if src_traj.exists():
                    shutil.copy2(src_traj, dst_traj)
                    traj_success = True
                else:
                    print(f"  -> [FAIL] Trajectory snapshot failed for {base_name}")
            except subprocess.TimeoutExpired:
                print(f"  -> [FAIL] Trajectory capture timeout for {base_name}")
                
        if seq_success and traj_success:
            print(f"  -> [OK] Generated baselines for {base_name}")
            success_count += 1
        else:
            fail_count += 1
            
    print(f"\n=== Batch Generation Complete ===")
    print(f"Successful: {success_count}/{len(seq_items)}")
    print(f"Failed:     {fail_count}/{len(seq_items)}")
    print(f"Output saved to: {out_dir.absolute()}")

if __name__ == "__main__":
    main()
