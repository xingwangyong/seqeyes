import argparse
import csv
import math
import os
import subprocess
import sys
from bisect import bisect_left
from pathlib import Path


def read_csv(path: Path, has_header: bool):
    t, x, y, z, n = [], [], [], [], []
    with path.open("r", newline="", encoding="utf-8") as f:
        r = csv.reader(f)
        if has_header:
            next(r, None)
        for row in r:
            if len(row) < 5:
                continue
            t.append(float(row[0]))
            x.append(float(row[1]))
            y.append(float(row[2]))
            z.append(float(row[3]))
            n.append(float(row[4]))
    return t, x, y, z, n


def interp1(xs, ys, x):
    if not xs:
        return float("nan")
    if x <= xs[0]:
        return ys[0]
    if x >= xs[-1]:
        return ys[-1]
    i = bisect_left(xs, x)
    x0, x1 = xs[i - 1], xs[i]
    y0, y1 = ys[i - 1], ys[i]
    if x1 == x0:
        return y0
    a = (x - x0) / (x1 - x0)
    return y0 + (y1 - y0) * a


def best_lag(a, b, max_lag=200):
    best_err = float("inf")
    best = 0
    for lag in range(-max_lag, max_lag + 1):
        if lag >= 0:
            aa = a[lag:]
            bb = b[: len(aa)]
        else:
            bb = b[-lag:]
            aa = a[: len(bb)]
        L = min(len(aa), len(bb))
        if L < 100:
            continue
        mse = sum((aa[i] - bb[i]) ** 2 for i in range(L)) / L
        if mse < best_err:
            best_err = mse
            best = lag
    return best


def compare(series_ref, series_cmp):
    _, xr, yr, zr, nr = series_ref
    _, xc, yc, zc, nc = series_cmp

    lag = best_lag(nr, nc, max_lag=300)
    if lag >= 0:
        xr2, yr2, zr2, nr2 = xr[lag:], yr[lag:], zr[lag:], nr[lag:]
        xc2, yc2, zc2, nc2 = xc[: len(xr2)], yc[: len(yr2)], zc[: len(zr2)], nc[: len(nr2)]
    else:
        xc2, yc2, zc2, nc2 = xc[-lag:], yc[-lag:], zc[-lag:], nc[-lag:]
        xr2, yr2, zr2, nr2 = xr[: len(xc2)], yr[: len(yc2)], zr[: len(zc2)], nr[: len(nc2)]

    L = min(len(xr2), len(xc2), len(yr2), len(yc2), len(zr2), len(zc2), len(nr2), len(nc2))
    xr2, yr2, zr2, nr2 = xr2[:L], yr2[:L], zr2[:L], nr2[:L]
    xc2, yc2, zc2, nc2 = xc2[:L], yc2[:L], zc2[:L], nc2[:L]

    errs = {
        "x": [xr2[i] - xc2[i] for i in range(L)],
        "y": [yr2[i] - yc2[i] for i in range(L)],
        "z": [zr2[i] - zc2[i] for i in range(L)],
        "norm": [nr2[i] - nc2[i] for i in range(L)],
    }

    out = {"lag_samples": lag, "aligned_samples": L}
    for k, e in errs.items():
        abs_e = [abs(v) for v in e]
        rmse = math.sqrt(sum(v * v for v in e) / max(1, len(e)))
        out[k] = {"max_abs": max(abs_e) if abs_e else 0.0, "rmse": rmse}
    return out


def main():
    ap = argparse.ArgumentParser(description="Compare SeqEyes PNS vs MATLAB seq.calcPNS on one .seq file.")
    ap.add_argument("--bin-dir", required=True, help="Directory containing PnsDumpTest.exe")
    ap.add_argument("--seq", required=True, help="Path to sequence file (.seq)")
    ap.add_argument("--asc", required=True, help="Path to hardware ASC file")
    ap.add_argument("--pulseq-matlab-dir", required=True, help="Path to pulseq matlab root containing +mr")
    ap.add_argument("--out-dir", default="test/pns_compare", help="Output folder")
    ap.add_argument("--sample-stride", type=int, default=20, help="Use every N-th sample for both outputs to speed up comparison")
    ap.add_argument("--max-abs-threshold", type=float, default=1e-4, help="Fail if any channel max abs exceeds this (normalized units)")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    seqeyes_csv = out_dir / "seqeyes_pns.csv"
    matlab_csv = out_dir / "matlab_pns.csv"

    exe = Path(args.bin_dir) / "PnsDumpTest.exe"
    if not exe.exists():
        print(f"[FAIL] Missing executable: {exe}")
        return 2

    print("[1/3] Dump SeqEyes PNS...")
    stride = max(1, int(args.sample_stride))
    cmd_seqeyes = [str(exe), "--seq", str(Path(args.seq).resolve()), "--asc", str(Path(args.asc).resolve()), "--out", str(seqeyes_csv.resolve()), "--stride", str(stride)]
    run_env = os.environ.copy()
    bin_dir = Path(args.bin_dir).resolve()
    runtime_candidates = [
        bin_dir,
        bin_dir.parent.parent / "Release",
    ]
    runtime_paths = [str(p) for p in runtime_candidates if p.exists()]
    if runtime_paths:
        run_env["PATH"] = os.pathsep.join(runtime_paths + [run_env.get("PATH", "")])
    cp = subprocess.run(cmd_seqeyes, text=True, capture_output=True, env=run_env)
    print(cp.stdout.strip())
    if cp.returncode != 0:
        print(cp.stderr)
        return cp.returncode

    print("[2/3] Dump MATLAB calcPNS...")
    env = os.environ.copy()
    env["PULSEQ_MATLAB_DIR"] = str(Path(args.pulseq_matlab_dir).resolve())
    env["SEQ_PATH"] = str(Path(args.seq).resolve())
    env["ASC_PATH"] = str(Path(args.asc).resolve())
    env["OUT_CSV"] = str(matlab_csv.resolve())
    env["SAMPLE_STRIDE"] = str(stride)
    script = Path("test/tools/matlab_calc_pns_cli.m").resolve()
    script_path_fwd = str(script).replace('\\', '/')
    cmd_matlab = ["matlab", "-batch", f"run('{script_path_fwd}')"] 
    cp2 = subprocess.run(cmd_matlab, capture_output=True, env=env)
    print(cp2.stdout.decode(errors='replace').strip())
    if cp2.returncode != 0:
        print(cp2.stderr.decode(errors='replace'))
        return cp2.returncode

    print("[3/3] Compare...")
    seqeyes = read_csv(seqeyes_csv, has_header=True)
    matlab = read_csv(matlab_csv, has_header=False)
    stats = compare(seqeyes, matlab)
    print(f"Aligned by lag={stats['lag_samples']} samples, N={stats['aligned_samples']}")

    print("Channel       max_abs        rmse")
    worst = 0.0
    for k in ("x", "y", "z", "norm"):
        m = stats[k]["max_abs"]
        r = stats[k]["rmse"]
        worst = max(worst, m)
        print(f"{k:7s}  {m:12.6g}  {r:12.6g}")

    if worst > args.max_abs_threshold:
        print(f"[FAIL] max_abs={worst:.6g} > threshold={args.max_abs_threshold:.6g}")
        return 1
    print(f"[PASS] max_abs={worst:.6g} <= threshold={args.max_abs_threshold:.6g}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
