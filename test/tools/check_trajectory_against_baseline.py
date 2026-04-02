import argparse
import math
import re
import subprocess
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    yaml = None


def eprint(msg: str):
    print(msg, file=sys.stderr)


def load_cases(cases_path: Path):
    if yaml is None:
        eprint("[ERROR] Missing dependency: pyyaml")
        sys.exit(2)
    if not cases_path.exists():
        eprint(f"[ERROR] Cases file not found: {cases_path}")
        sys.exit(1)

    with open(cases_path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}

    defaults = data.get("defaults", {})
    default_thr = (defaults.get("thresholds") or {})
    cases = data.get("cases", [])
    if not isinstance(cases, list) or not cases:
        eprint("[ERROR] Cases file has no valid 'cases' list")
        sys.exit(1)

    out = []
    for c in cases:
        cid = str(c.get("id", "")).strip()
        seq = str(c.get("seq", "")).strip()
        if not cid or not seq:
            eprint(f"[ERROR] Invalid case entry: {c}")
            sys.exit(1)
        thr = dict(default_thr)
        thr.update(c.get("thresholds") or {})
        out.append(
            {
                "id": cid,
                "seq": seq,
                "thresholds": {
                    "max_abs": float(thr.get("max_abs", 1e-5)),
                    "rmse": float(thr.get("rmse", 1e-5)),
                    "mean_abs": float(thr.get("mean_abs", 5e-6)),
                },
            }
        )
    return out


def find_seqeyes_exe(bin_dir: Path) -> Path:
    candidates = [
        bin_dir / "seqeyes.exe",
        bin_dir / "SeqEyes.exe",
        bin_dir / "Release" / "seqeyes.exe",
        bin_dir / "Release" / "SeqEyes.exe",
    ]
    for p in candidates:
        if p.exists():
            return p
    eprint(f"[ERROR] seqeyes.exe not found in {bin_dir}")
    sys.exit(1)


def run_cmd(cmd, cwd=None):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def write_automation_json(path: Path, seq_abs: Path, out_dir_abs: Path):
    import json

    scenario = {
        "actions": [
            {"type": "open_file", "path": seq_abs.as_posix()},
            {"type": "export_trajectory", "dir": out_dir_abs.as_posix()},
        ]
    }
    path.write_text(json.dumps(scenario, ensure_ascii=False), encoding="utf-8")


def read_matrix(path: Path):
    if not path.exists():
        raise FileNotFoundError(str(path))
    rows = []
    for ln in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        t = ln.strip()
        if not t:
            continue
        toks = [x for x in re.split(r"[,\s]+", t) if x]
        vals = [float(x) for x in toks]
        rows.append(vals)
    if not rows:
        return []
    ncol = max(len(r) for r in rows)
    rows = [r for r in rows if len(r) == ncol]
    return rows


def normalize_to_nx3(mat):
    if not mat:
        return []
    nr = len(mat)
    nc = len(mat[0])
    if nc == 3:
        return mat
    if nr == 3 and nc > 3:
        out = []
        for j in range(nc):
            out.append([mat[0][j], mat[1][j], mat[2][j]])
        return out
    raise ValueError(f"Unsupported matrix shape: {nr}x{nc}, expected Nx3 or 3xN")


def compute_stats(cur_adc, base_adc):
    n = min(len(cur_adc), len(base_adc))
    if n <= 0:
        raise ValueError("No samples to compare")
    d = [[cur_adc[i][k] - base_adc[i][k] for k in range(3)] for i in range(n)]
    max_abs = [max(abs(row[k]) for row in d) for k in range(3)]
    rmse = []
    mean = []
    for k in range(3):
        s2 = sum(row[k] * row[k] for row in d)
        s1 = sum(row[k] for row in d)
        rmse.append(math.sqrt(s2 / n))
        mean.append(s1 / n)
    return {
        "n_cur": len(cur_adc),
        "n_base": len(base_adc),
        "n_used": n,
        "max_abs": max_abs,
        "rmse": rmse,
        "mean": mean,
    }


def pass_threshold(stats, thr):
    for k in range(3):
        if stats["max_abs"][k] > thr["max_abs"]:
            return False
        if stats["rmse"][k] > thr["rmse"]:
            return False
        if abs(stats["mean"][k]) > thr["mean_abs"]:
            return False
    return True


def main():
    ap = argparse.ArgumentParser(description="Compare exported trajectory (ktraj_adc) against baseline txt files.")
    ap.add_argument("--bin-dir", required=True, help="Directory containing seqeyes.exe")
    ap.add_argument("--cases", default="test/trajectory_regression_cases.yaml", help="YAML cases file")
    ap.add_argument("--baseline-root", required=True, help="Extracted baseline root directory")
    ap.add_argument("--out-dir", default="test/trajectory_regression_ci_out", help="Output working directory")
    ap.add_argument("--only", default="", help="Comma-separated case ids to run")
    args = ap.parse_args()

    repo_root = Path.cwd().resolve()
    exe = find_seqeyes_exe(Path(args.bin_dir))
    cases = load_cases(Path(args.cases))
    baseline_root = Path(args.baseline_root).resolve()
    out_root = Path(args.out_dir).resolve()
    out_root.mkdir(parents=True, exist_ok=True)

    only_set = set(x.strip() for x in args.only.split(",") if x.strip())
    if only_set:
        cases = [c for c in cases if c["id"] in only_set]
        if not cases:
            eprint("[ERROR] --only filtered out all cases")
            sys.exit(1)

    print(f"Using seqeyes: {exe}")
    print(f"Baseline root: {baseline_root}")
    print(f"Cases: {len(cases)}")

    any_fail = False
    for c in cases:
        cid = c["id"]
        thr = c["thresholds"]
        seq_abs = (repo_root / c["seq"]).resolve()
        case_out = (out_root / cid).resolve()
        case_out.mkdir(parents=True, exist_ok=True)
        export_dir = case_out / "export_cpp"
        export_dir.mkdir(parents=True, exist_ok=True)
        scen_path = case_out / "scenario.json"

        base_adc_path = baseline_root / cid / "export" / "ktraj_adc.txt"

        print(f"\n--- [{cid}] ---")
        print(f"seq: {seq_abs}")
        print(f"baseline_adc: {base_adc_path}")

        if not seq_abs.exists():
            print(f"[FAIL] missing seq: {seq_abs}")
            any_fail = True
            continue
        if not base_adc_path.exists():
            print(f"[FAIL] missing baseline: {base_adc_path}")
            any_fail = True
            continue

        write_automation_json(scen_path, seq_abs, export_dir)
        rc, so, se = run_cmd([str(exe), "--automation", str(scen_path)], cwd=repo_root)
        if rc != 0:
            print(f"[FAIL] seqeyes automation failed (rc={rc})")
            if so.strip():
                print(so.strip())
            if se.strip():
                print(se.strip())
            any_fail = True
            continue

        cur_adc_path = export_dir / "ktraj_adc.txt"
        if not cur_adc_path.exists():
            print(f"[FAIL] missing current export: {cur_adc_path}")
            any_fail = True
            continue

        try:
            cur_adc = normalize_to_nx3(read_matrix(cur_adc_path))
            base_adc = normalize_to_nx3(read_matrix(base_adc_path))
            stats = compute_stats(cur_adc, base_adc)
        except Exception as ex:
            print(f"[FAIL] parse/compare error: {ex}")
            any_fail = True
            continue

        ok = pass_threshold(stats, thr)
        print(
            f"rows: current={stats['n_cur']} baseline={stats['n_base']} used={stats['n_used']}\n"
            f"max_abs=[{stats['max_abs'][0]:.9g} {stats['max_abs'][1]:.9g} {stats['max_abs'][2]:.9g}]\n"
            f"rmse   =[{stats['rmse'][0]:.9g} {stats['rmse'][1]:.9g} {stats['rmse'][2]:.9g}]\n"
            f"mean   =[{stats['mean'][0]:.9g} {stats['mean'][1]:.9g} {stats['mean'][2]:.9g}]"
        )
        print(f"thresholds: max_abs<={thr['max_abs']}, rmse<={thr['rmse']}, abs(mean)<={thr['mean_abs']}")
        if ok:
            print("[PASS]")
        else:
            print("[FAIL] threshold exceeded")
            any_fail = True

    if any_fail:
        print("\n[FAILED] Trajectory baseline regression has failures.")
        sys.exit(1)
    print("\n[SUCCESS] Trajectory baseline regression passed.")
    sys.exit(0)


if __name__ == "__main__":
    main()
