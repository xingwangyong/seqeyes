import argparse
import subprocess
import sys
import json
import tempfile
import os
import time
from datetime import datetime, timezone
from pathlib import Path


def detect_exe(bin_dir: Path) -> Path:
    """Prefer SeqEyes.exe (automation mode), fallback to legacy SeqEye and then PerfZoomTest.exe."""
    candidates = [
        bin_dir / "SeqEyes.exe",
        bin_dir / "SeqEyes",
        bin_dir / "test" / "SeqEyes.exe",
        bin_dir / "test" / "SeqEyes",
        bin_dir / "SeqEye.exe",
        bin_dir / "SeqEye",
        bin_dir / "test" / "SeqEye.exe",
        bin_dir / "test" / "SeqEye",
        bin_dir / "PerfZoomTest.exe",
        bin_dir / "test" / "PerfZoomTest.exe",
        bin_dir / "PerfZoomTest",
        bin_dir / "test" / "PerfZoomTest",
    ]
    for c in candidates:
        if c.exists():
            return c
    raise FileNotFoundError(f"Neither SeqEyes/SeqEye nor PerfZoomTest found under {bin_dir}")


def parse_zoom_ms(stdout: str):
    """Extract zoom time from PerfZoomTest stdout. Returns float or None."""
    for line in stdout.splitlines():
        if line.startswith("ZOOM_MS:"):
            try:
                return float(line.split(":", 1)[1].strip())
            except Exception:
                return None
    return None


def run_one(exe: Path, seq_path: Path):
    """Run a single measurement. If SeqEye is detected, use --automation; otherwise fallback to PerfZoomTest.
    For SeqEye, stream stdout and kill process after ZOOM_MS is captured to avoid long teardown on huge files.
    """
    exe_name = exe.name.lower()
    seq_abs = seq_path.resolve()
    if "seqeye" in exe_name:
        scenario = {
            "actions": [
                {"type": "open_file", "path": seq_abs.as_posix()},
                {"type": "reset_view"},
                {"type": "measure_zoom_by_factor", "factor": 0.5},
            ]
        }
        with tempfile.NamedTemporaryFile(delete=False, suffix=".json") as tf:
            tf.write(json.dumps(scenario).encode("utf-8"))
            scen_path = tf.name
        cmd = [str(exe), "--automation", scen_path]

        # Stream stdout and wait for natural exit (measure end-to-end latency on the real interaction path)
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        out_lines = []
        err_lines = []
        zoom_ms = None
        while True:
            if proc.stdout is None:
                break
            line = proc.stdout.readline()
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.01)
                continue
            out_lines.append(line)
            if line.startswith("ZOOM_MS:") and zoom_ms is None:
                try:
                    zoom_ms = float(line.split(":", 1)[1].strip())
                except Exception:
                    zoom_ms = None
        # drain stderr
        if proc.stderr is not None:
            err_lines = proc.stderr.read().splitlines()
        rc = proc.wait()
        return rc, "".join(out_lines), "\n".join(err_lines), zoom_ms
    else:
        cmd = [str(exe), "--seq", str(seq_abs)]
        p = subprocess.run(cmd, capture_output=True, text=True)
        zoom_ms = parse_zoom_ms(p.stdout)
        return p.returncode, p.stdout, p.stderr, zoom_ms


DEFAULT_TEST_DIR = Path(__file__).resolve().parents[0]
DEFAULT_BASELINE = DEFAULT_TEST_DIR / "perf_baseline.json"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin-dir", type=Path, required=True, help="Directory containing built test executables")
    ap.add_argument("--seq", type=Path, default=None, help="Optional .seq file to use")
    ap.add_argument("--seq-dir", type=Path, default=None, help="Directory containing .seq files to test (non-recursive)")
    ap.add_argument("--out", type=Path, default=Path("perf_results.json"), help="Path to write aggregated results JSON")
    ap.add_argument("--github-benchmark-out", type=Path, default=None, help="Path to write GitHub Benchmark format JSON")
    ap.add_argument("--repeat", type=int, default=1, help="Number of times to repeat each test (for statistical stability)")
    ap.add_argument("--warmup", action="store_true", help="Run once before starting measurements to warm up caches")
    ap.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE, help="Baseline JSON (default: test/perf_baseline.json if exists)")
    ap.add_argument("--threshold-ms", type=float, default=None, help="Absolute regression threshold in ms; if omitted, use 10% of baseline")
    args = ap.parse_args()

    exe = detect_exe(args.bin_dir)

    # Auto default seq-dir to repo test/seq_files if not provided
    if args.seq is None and args.seq_dir is None:
        default_dir = Path(__file__).resolve().parents[0] / "seq_files"
        if default_dir.exists():
            args.seq_dir = default_dir
        else:
            print("Must provide either --seq or --seq-dir.")
            sys.exit(2)

    overall_fail = 0
    results = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "exe": str(exe),
        "entries": []
    }

    def decode_exit(rc: int):
        # Windows NTSTATUS common codes; extend as needed
        reasons = {
            0xC0000005: "Access Violation",
            0xC0000135: "DLL Not Found",
            0xC0000139: "Entry Point Not Found",
            0xC0000142: "DLL Initialization Failed",
            0xC00000FD: "Stack Overflow",
            0xC0000409: "Stack Buffer Overrun",
            0xC000001D: "Illegal Instruction",
        }
        if os.name == 'nt' and rc >= 0xC0000000:
            hexv = f"0x{rc:08X}"
            return hexv, reasons.get(rc, "Unknown NTSTATUS")
        return None, None

    import statistics

    def run_multi(exe: Path, seq_path: Path, count: int, warmup: bool):
        if warmup:
            print(f"  [Warmup] {seq_path.name}...")
            run_one(exe, seq_path)
        
        times = []
        last_rc = 0
        last_out = ""
        last_err = ""
        
        for i in range(count):
            label = f"Iteration {i+1}/{count}" if count > 1 else "Running"
            rc, out, err, zoom_ms = run_one(exe, seq_path)
            last_rc, last_out, last_err = rc, out, err
            if rc != 0:
                return rc, out, err, None, []
            if zoom_ms is not None:
                times.append(zoom_ms)
                if count > 1:
                    print(f"    {label}: {zoom_ms:.2f} ms")
            else:
                return 1, out, err, None, times
        
        median_ms = statistics.median(times) if times else None
        return last_rc, last_out, last_err, median_ms, times

    # Single-file mode
    if args.seq is not None:
        seq_abs = args.seq.resolve()
        print("Running single file:", seq_abs)
        rc, out, err, median_ms, all_times = run_multi(exe, seq_abs, args.repeat, args.warmup)
        sys.stdout.write(out)
        sys.stderr.write(err)
        hexv, reason = decode_exit(rc)
        entry = {"file": str(seq_abs), "zoom_ms": median_ms, "exit": rc, "runs": all_times}
        if hexv:
            entry["exit_hex"] = hexv
            entry["exit_reason"] = reason
        results["entries"].append(entry)
        if rc != 0:
            msg = f"Exit code: {rc}"
            if hexv:
                msg += f" ({hexv} {reason})"
            if median_ms is not None:
                msg += f"; median={median_ms:.2f} ms then crash"
            print(msg)
            overall_fail = 1
        elif median_ms is None:
            print("[FAIL] Did not find valid ZOOM_MS in output")
            overall_fail = 1
        else:
            if args.repeat > 1:
                print(f"[OK] Median Zoom-in time: {median_ms:.2f} ms (from {len(all_times)} runs)")
            else:
                print(f"[OK] Zoom-in time: {median_ms:.2f} ms")

    # Directory mode
    if args.seq_dir is not None:
        if not args.seq_dir.exists() or not args.seq_dir.is_dir():
            print(f"[FAIL] --seq-dir not found or not a directory: {args.seq_dir}")
            sys.exit(2)
        seq_files = sorted(args.seq_dir.resolve().glob("*.seq"))
        if not seq_files:
            print(f"[FAIL] No .seq files under {args.seq_dir}")
            sys.exit(2)
        print(f"Found {len(seq_files)} .seq files under {args.seq_dir}")
        for seq_file in seq_files:
            seq_abs = seq_file.resolve()
            print(f"Testing {seq_abs.name}...")
            rc, out, err, median_ms, all_times = run_multi(exe, seq_abs, args.repeat, args.warmup)
            hexv, reason = decode_exit(rc)
            entry = {"file": str(seq_abs), "zoom_ms": median_ms, "exit": rc, "runs": all_times}
            if hexv:
                entry["exit_hex"] = hexv
                entry["exit_reason"] = reason
            results["entries"].append(entry)
            if rc != 0:
                line = f"{seq_abs.name}: [FAIL] exit {rc}"
                if hexv:
                    line += f" ({hexv} {reason})"
                if median_ms is not None:
                    line += f"; median={median_ms:.2f} ms then crash"
                print(line)
                overall_fail = 1
                continue
            if median_ms is None:
                print(f"{seq_abs.name}: [FAIL] invalid or missing ZOOM_MS")
                overall_fail = 1
            else:
                if args.repeat > 1:
                    print(f"{seq_abs.name}: median {median_ms:.2f} ms")
                else:
                    print(f"{seq_abs.name}: {median_ms:.2f} ms")

    # Save results
    try:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(results, indent=2), encoding="utf-8")
        print(f"Saved results to {args.out}")
    except Exception as e:
        print(f"[WARN] Failed to save results: {e}")

    # Save format for github-action-benchmark
    if args.github_benchmark_out:
        try:
            bench_data = []
            for e in results["entries"]:
                name = Path(e["file"]).name
                val = e.get("zoom_ms")
                if val is not None:
                    bench_data.append({
                        "name": f"Zoom Performance: {name}",
                        "unit": "ms",
                        "value": val
                    })
            args.github_benchmark_out.parent.mkdir(parents=True, exist_ok=True)
            args.github_benchmark_out.write_text(json.dumps(bench_data, indent=2), encoding="utf-8")
            print(f"Saved GitHub Benchmark data to {args.github_benchmark_out}")
        except Exception as e:
            print(f"[WARN] Failed to save benchmark results: {e}")

    # Compare with baseline if provided
    if args.baseline and args.baseline.exists():
        try:
            baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
            # Build map filename->zoom_ms
            base_map = {Path(e["file"]).name: e.get("zoom_ms") for e in baseline.get("entries", [])}
            cur_map = {Path(e["file"]).name: e.get("zoom_ms") for e in results.get("entries", [])}
            for name, cur in cur_map.items():
                base = base_map.get(name)
                if base is None or cur is None:
                    continue
                allowed = args.threshold_ms if args.threshold_ms is not None else (0.1 * base)
                if (cur - base) > allowed:
                    if args.threshold_ms is not None:
                        print(f"[REGRESSION] {name}: {base:.2f} -> {cur:.2f} ms (+{cur-base:.2f} ms > {allowed:.2f} ms)")
                    else:
                        print(f"[REGRESSION] {name}: {base:.2f} -> {cur:.2f} ms (+{cur-base:.2f} ms > 10%={allowed:.2f} ms)")
                    overall_fail = 1
        except Exception as e:
            print(f"[WARN] Failed to compare baseline: {e}")

    sys.exit(overall_fail)


if __name__ == "__main__":
    main()

