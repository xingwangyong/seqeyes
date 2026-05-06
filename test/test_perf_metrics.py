import argparse
import atexit
import csv
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

_DUMMY_PNS_ASC_CONTENT = """# Temporary dummy ASC profile used only for local performance testing.
flGSWDTauX[0] = 0.20
flGSWDTauX[1] = 3.00
flGSWDTauX[2] = 20.0
flGSWDAX[0] = 0.55
flGSWDAX[1] = 0.30
flGSWDAX[2] = 0.15
flGSWDStimulationLimitX = 30.0
flGSWDStimulationThresholdX = 24.0
flGScaleFactorX = 1.00

flGSWDTauY[0] = 0.18
flGSWDTauY[1] = 2.80
flGSWDTauY[2] = 18.0
flGSWDAY[0] = 0.52
flGSWDAY[1] = 0.33
flGSWDAY[2] = 0.15
flGSWDStimulationLimitY = 28.0
flGSWDStimulationThresholdY = 22.4
flGScaleFactorY = 1.00

flGSWDTauZ[0] = 0.22
flGSWDTauZ[1] = 3.20
flGSWDTauZ[2] = 24.0
flGSWDAZ[0] = 0.57
flGSWDAZ[1] = 0.28
flGSWDAZ[2] = 0.15
flGSWDStimulationLimitZ = 26.0
flGSWDStimulationThresholdZ = 20.8
flGScaleFactorZ = 1.00
"""

METRIC_KEYS = [
    "LOAD_MS",
    "ZOOM_WAVEFORM_MS",
    "TRAJECTORY_REFRESH_MS",
]

METRIC_OUTPUT_NAMES = {
    "LOAD_MS": "load_ms",
    "ZOOM_WAVEFORM_MS": "worst_case_first_zoom_ms",
    "TRAJECTORY_REFRESH_MS": "synchronized_trajectory_update_ms",
}


def create_temp_dummy_pns_asc() -> Path:
    f = tempfile.NamedTemporaryFile(delete=False, suffix=".asc", mode="w", encoding="utf-8")
    f.write(_DUMMY_PNS_ASC_CONTENT)
    f.flush()
    f.close()
    return Path(f.name)


def detect_exe(bin_dir: Path) -> Path:
    candidates = [
        bin_dir / "SeqEyes.exe",
        bin_dir / "SeqEyes",
        bin_dir / "test" / "SeqEyes.exe",
        bin_dir / "test" / "SeqEyes",
        bin_dir / "SeqEye.exe",
        bin_dir / "SeqEye",
        bin_dir / "test" / "SeqEye.exe",
        bin_dir / "test" / "SeqEye",
    ]
    for c in candidates:
        if c.exists():
            return c
    raise FileNotFoundError(f"SeqEyes/SeqEye not found under {bin_dir}")


def parse_metrics(stdout: str) -> dict[str, float]:
    metrics: dict[str, float] = {}
    for line in stdout.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        key = key.strip()
        if key not in METRIC_KEYS:
            continue
        try:
            metrics[key] = float(value.strip())
        except ValueError:
            pass
    return metrics


def parse_sequence_metadata(stdout: str) -> dict[str, float | int]:
    meta: dict[str, float | int] = {}

    blocks_match = re.search(r"(\d+)\s+blocks detected!", stdout)
    if blocks_match:
        meta["blocks"] = int(blocks_match.group(1))

    duration_match = re.search(r"Sequence total duration:\s*([0-9]+(?:\.[0-9]+)?)\s*seconds", stdout)
    if duration_match:
        meta["duration_s"] = float(duration_match.group(1))

    return meta


def round1(value):
    if value is None:
        return None
    return round(float(value), 1)


def median_only(values: list[float]) -> float | None:
    if not values:
        return None
    return statistics.median(values)


def build_scenario(seq_abs: Path) -> dict:
    scenario = {"actions": [{"type": "open_file", "path": seq_abs.as_posix()}]}
    pns_asc = os.environ.get("SEQEYES_PERF_PNS_ASC", "").strip()
    if pns_asc:
        scenario["actions"].append(
            {
                "type": "configure_pns",
                "asc_path": str(Path(pns_asc).resolve().as_posix()),
                "show_pns": True,
                "show_x": True,
                "show_y": True,
                "show_z": True,
                "show_norm": True,
            }
        )
    scenario["actions"].extend(
        [
            {"type": "reset_view"},
            {"type": "set_trajectory_visible", "show": False},
            # Measure zoom from the initial full-sequence overview after reset_view.
            # This is intentionally a worst-case / first-zoom benchmark, because the
            # visible span is maximal here and later zoom-in steps are typically cheaper.
            {"type": "measure_zoom_by_factor", "factor": 0.5},
            {"type": "set_trajectory_visible", "show": True},
            # Measure the synchronized trajectory panel update separately, using the
            # waveform viewport produced by the current interaction state.
            {"type": "measure_trajectory_refresh"},
        ]
    )
    return scenario


def run_one(exe: Path, seq_path: Path) -> tuple[int, str, str, dict[str, float], dict[str, float | int]]:
    scenario = build_scenario(seq_path.resolve())
    with tempfile.NamedTemporaryFile(delete=False, suffix=".json") as tf:
        tf.write(json.dumps(scenario).encode("utf-8"))
        scen_path = tf.name
    try:
        proc = subprocess.run([str(exe), "--automation", scen_path], capture_output=True, text=True)
    finally:
        Path(scen_path).unlink(missing_ok=True)
    return proc.returncode, proc.stdout, proc.stderr, parse_metrics(proc.stdout), parse_sequence_metadata(proc.stdout)


def run_multi(exe: Path, seq_path: Path, repeat: int, warmup: bool):
    if warmup:
        run_one(exe, seq_path)

    runs = []
    metadata: dict[str, float | int] = {}
    last_stdout = ""
    last_stderr = ""
    last_rc = 0
    for _ in range(repeat):
        rc, stdout, stderr, metrics, run_metadata = run_one(exe, seq_path)
        last_rc = rc
        last_stdout = stdout
        last_stderr = stderr
        if run_metadata:
            metadata = run_metadata
        if rc != 0:
            return rc, stdout, stderr, [], metadata
        missing = [k for k in METRIC_KEYS if k not in metrics]
        if missing:
            return 1, stdout, f"Missing metrics: {', '.join(missing)}\n{stderr}".strip(), [], metadata
        runs.append(metrics)
    return last_rc, last_stdout, last_stderr, runs, metadata


def summarize_runs(runs: list[dict[str, float]]) -> dict:
    summary = {"runs": runs}
    for key in METRIC_KEYS:
        values = [r[key] for r in runs if key in r]
        summary[METRIC_OUTPUT_NAMES[key]] = {
            "median_ms": round1(median_only(values)),
        }
    return summary


def main():
    ap = argparse.ArgumentParser(description="Measure SeqEyes load time, worst-case first zoom latency, and synchronized trajectory update time on one or more .seq files.")
    ap.add_argument("--bin-dir", type=Path, required=True, help="Directory containing SeqEyes executable")
    ap.add_argument("--seq", type=Path, action="append", default=None, help="Specific .seq file to test; may be repeated")
    ap.add_argument("--seq-dir", type=Path, default=None, help="Directory containing .seq files to test (non-recursive)")
    ap.add_argument("--repeat", type=int, default=5, help="Number of times to repeat each test")
    ap.add_argument("--warmup", action="store_true", help="Run one warmup iteration before measurement")
    ap.add_argument("--out-json", type=Path, default=Path("perf_metrics_results.json"), help="Path to write JSON results")
    ap.add_argument("--out-csv", type=Path, default=Path("perf_metrics_results.csv"), help="Path to write CSV summary")
    ap.add_argument("--pns-asc", type=Path, default=None, help="Optional ASC profile path for worst-case PNS timing")
    ap.add_argument("--use-dummy-pns-asc", action="store_true", help="Use a generated dummy ASC profile for local worst-case timing")
    args = ap.parse_args()

    temp_dummy_asc = None
    if args.use_dummy_pns_asc:
        temp_dummy_asc = create_temp_dummy_pns_asc()
        os.environ["SEQEYES_PERF_PNS_ASC"] = str(temp_dummy_asc.resolve())
        atexit.register(lambda p=temp_dummy_asc: p.unlink(missing_ok=True))
    elif args.pns_asc is not None:
        asc = args.pns_asc.resolve()
        if not asc.exists():
            print(f"[FAIL] --pns-asc not found: {asc}")
            sys.exit(2)
        os.environ["SEQEYES_PERF_PNS_ASC"] = str(asc)

    exe = detect_exe(args.bin_dir)

    seq_files: list[Path] = []
    if args.seq:
        seq_files.extend([p.resolve() for p in args.seq])
    if args.seq_dir:
        if not args.seq_dir.exists() or not args.seq_dir.is_dir():
            print(f"[FAIL] --seq-dir not found or not a directory: {args.seq_dir}")
            sys.exit(2)
        seq_files.extend(sorted(p.resolve() for p in args.seq_dir.glob("*.seq")))
    if not seq_files:
        print("[FAIL] Provide --seq and/or --seq-dir")
        sys.exit(2)

    seen = set()
    deduped = []
    for seq in seq_files:
        if seq not in seen:
            deduped.append(seq)
            seen.add(seq)
    seq_files = deduped

    results = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "exe": str(exe),
        "repeat": args.repeat,
        "entries": [],
    }
    csv_rows = []
    overall_fail = 0

    for seq in seq_files:
        print(f"Testing {seq.name} ...")
        rc, stdout, stderr, runs, metadata = run_multi(exe, seq, args.repeat, args.warmup)
        file_size_mb = round1(seq.stat().st_size / (1024.0 * 1024.0))
        if rc != 0:
            print(f"[FAIL] {seq.name} (exit={rc})")
            if stdout.strip():
                print(stdout.strip())
            if stderr.strip():
                print(stderr.strip())
            overall_fail = 1
            results["entries"].append({
                "file": str(seq),
                "file_size_mb": file_size_mb,
                "blocks": metadata.get("blocks"),
                "duration_s": round1(metadata.get("duration_s")),
                "exit": rc,
                "stdout": stdout,
                "stderr": stderr,
            })
            continue

        summary = summarize_runs(runs)
        entry = {
            "file": str(seq),
            "file_size_mb": file_size_mb,
            "blocks": metadata.get("blocks"),
            "duration_s": round1(metadata.get("duration_s")),
            "exit": 0,
            **summary,
        }
        results["entries"].append(entry)

        row = {
            "file": str(seq),
            "name": seq.name,
            "file_size_mb": file_size_mb,
            "blocks": entry.get("blocks"),
            "duration_s": entry.get("duration_s"),
        }
        for key in METRIC_KEYS:
            metric_name = METRIC_OUTPUT_NAMES[key]
            metric = entry[metric_name]
            row[f"{metric_name}_median_ms"] = metric["median_ms"]
        csv_rows.append(row)

        print(
            f"  load={entry['load_ms']['median_ms']:.1f} ms, "
            f"worst-case-zoom={entry['worst_case_first_zoom_ms']['median_ms']:.1f} ms, "
            f"sync-trajectory={entry['synchronized_trajectory_update_ms']['median_ms']:.1f} ms"
        )

    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"Saved JSON results to {args.out_json}")

    args.out_csv.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "file",
        "name",
        "file_size_mb",
        "blocks",
        "duration_s",
        "load_ms_median_ms",
        "worst_case_first_zoom_ms_median_ms",
        "synchronized_trajectory_update_ms_median_ms",
    ]
    with args.out_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(csv_rows)
    print(f"Saved CSV summary to {args.out_csv}")

    sys.exit(overall_fail)


if __name__ == "__main__":
    main()
