import os
import sys
import subprocess
import argparse
import tempfile
import shutil
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description="Batch generate visual regression baselines (SVG) for SeqEyes.")
    parser.add_argument("--seq-dir", type=str, required=True, help="Directory containing .seq files")
    parser.add_argument("--bin-dir", type=str, required=True, help="Directory containing seqeyes.exe")
    parser.add_argument("--out-dir", type=str, required=True, help="Directory to save the generated baselines")

    args = parser.parse_args()

    seq_dir = Path(args.seq_dir)
    bin_dir = Path(args.bin_dir)
    out_dir = Path(args.out_dir)

    exe_path = bin_dir / "seqeyes.exe"
    if not exe_path.exists():
        print(f"[ERROR] Executable not found at {exe_path}")
        sys.exit(1)

    if not seq_dir.exists() or not seq_dir.is_dir():
        print(f"[ERROR] Sequence directory not found: {seq_dir}")
        sys.exit(1)

    out_dir.mkdir(parents=True, exist_ok=True)

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

    print(f"Found {len(seq_files)} sequence files from the target list. Generating baselines (SVG)...\n")

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

    for seq_file in seq_files:
        base_name = seq_file.stem
        print(f"[{base_name}] Processing...")

        # Both _seq.svg and _traj.svg are produced in a single seqeyes run.
        # The --capture-snapshots flag generates two SVG files: one for seq, one for traj.
        with tempfile.TemporaryDirectory() as tmp_dir:
            try:
                res = subprocess.run(
                    [str(exe_path), "--Whole-sequence", "--time-range", "0~10",
                     "--capture-snapshots", tmp_dir, str(seq_file)],
                    capture_output=True, text=True, timeout=120, env=qt_env
                )
                if res.returncode != 0:
                    print(f"  -> [CRASH] seqeyes.exe exited with code {res.returncode}. Stderr: {res.stderr.strip()[:300]}")

                src_seq = Path(tmp_dir) / f"{base_name}_seq.svg"
                src_traj = Path(tmp_dir) / f"{base_name}_traj.svg"

                seq_ok = src_seq.exists()
                traj_ok = src_traj.exists()

                if seq_ok:
                    shutil.copy2(src_seq, out_dir / f"{base_name}_seq.svg")
                else:
                    print(f"  -> [FAIL] Sequence SVG not found for {base_name}")

                if traj_ok:
                    shutil.copy2(src_traj, out_dir / f"{base_name}_traj.svg")
                else:
                    print(f"  -> [FAIL] Trajectory SVG not found for {base_name}")

                if seq_ok and traj_ok:
                    print(f"  -> [OK] Generated baselines for {base_name}")
                    success_count += 1
                else:
                    fail_count += 1

            except subprocess.TimeoutExpired:
                print(f"  -> [FAIL] Timeout for {base_name}")
                fail_count += 1

    print(f"\n=== Batch Generation Complete ===")
    print(f"Successful: {success_count}/{len(seq_files)}")
    print(f"Failed:     {fail_count}/{len(seq_files)}")
    print(f"Output saved to: {out_dir.absolute()}")

if __name__ == "__main__":
    main()
