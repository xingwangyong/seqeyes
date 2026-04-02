import argparse
import json
import math
import os
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


def expand_env_vars(s: str) -> str:
    if not isinstance(s, str):
        return s
    pattern = re.compile(r"\$\{([^}]+)\}")

    def repl(match):
        key = match.group(1)
        return os.environ.get(key, "")

    return pattern.sub(repl, s)


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
        seq = expand_env_vars(str(c.get("seq", "")).strip())
        pdir = expand_env_vars(str(c.get("pulseq_matlab_dir", "")).strip())
        if not cid or not seq or not pdir:
            eprint(f"[ERROR] Invalid case entry: {c}")
            sys.exit(1)
        thr = dict(default_thr)
        thr.update(c.get("thresholds") or {})
        out.append(
            {
                "id": cid,
                "seq": seq,
                "pulseq_matlab_dir": pdir,
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


def compute_stats(cpp_adc, matlab_adc):
    n = min(len(cpp_adc), len(matlab_adc))
    if n <= 0:
        raise ValueError("No samples to compare")
    d = [[cpp_adc[i][k] - matlab_adc[i][k] for k in range(3)] for i in range(n)]
    max_abs = [max(abs(row[k]) for row in d) for k in range(3)]
    rmse = []
    mean = []
    for k in range(3):
        s2 = sum(row[k] * row[k] for row in d)
        s1 = sum(row[k] for row in d)
        rmse.append(math.sqrt(s2 / n))
        mean.append(s1 / n)
    return {
        "n_cpp": len(cpp_adc),
        "n_matlab": len(matlab_adc),
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


def write_matlab_script(path: Path, pulseq_dir: Path, seq_abs: Path, out_adc_path: Path):
    out_abs = out_adc_path.resolve()
    code = f"""addpath('{pulseq_dir.as_posix()}');
seq = mr.Sequence();
seq.read('{seq_abs.as_posix()}');
[k_adc,~,~,~] = seq.calculateKspacePP();
out_path = '{out_abs.as_posix()}';
out_dir = fileparts(out_path);
if ~exist(out_dir, 'dir')
    mkdir(out_dir);
end
writematrix(k_adc', out_path, 'Delimiter',' ');
"""
    path.write_text(code, encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description="Trajectory regression against MATLAB calculateKspacePP()")
    ap.add_argument("--bin-dir", default="out/build/x64-Release/Release", help="Directory containing seqeyes.exe")
    ap.add_argument("--cases", default="test/trajectory_regression_cases.yaml", help="YAML cases file")
    ap.add_argument("--out-dir", default="test/trajectory_regression_out", help="Output working directory")
    ap.add_argument("--matlab-cmd", default="matlab", help="MATLAB executable command")
    ap.add_argument("--only", default="", help="Comma-separated case ids to run")
    ap.add_argument("--keep-temp", action="store_true", help="Keep per-case temp files")
    args = ap.parse_args()

    repo_root = Path.cwd()
    exe = find_seqeyes_exe(Path(args.bin_dir))
    cases = load_cases(Path(args.cases))
    out_root = Path(args.out_dir)
    out_root.mkdir(parents=True, exist_ok=True)

    only_set = set(x.strip() for x in args.only.split(",") if x.strip())
    if only_set:
        cases = [c for c in cases if c["id"] in only_set]
        if not cases:
            eprint("[ERROR] --only filtered out all cases")
            sys.exit(1)

    any_fail = False
    print(f"Using seqeyes: {exe}")
    print(f"Cases: {len(cases)}")

    for c in cases:
        cid = c["id"]
        seq_abs = (repo_root / c["seq"]).resolve()
        pulseq_dir = Path(c["pulseq_matlab_dir"]).resolve()
        thr = c["thresholds"]
        case_dir = (out_root / cid).resolve()
        case_dir.mkdir(parents=True, exist_ok=True)
        export_dir = (case_dir / "export_cpp").resolve()
        export_dir.mkdir(parents=True, exist_ok=True)
        scen_path = (case_dir / "scenario.json").resolve()

        print(f"\n--- [{cid}] ---")
        print(f"seq: {seq_abs}")
        print(f"pulseq matlab dir: {pulseq_dir}")

        if not seq_abs.exists():
            print(f"[FAIL] seq file missing: {seq_abs}")
            any_fail = True
            continue
        if not pulseq_dir.exists():
            print(f"[FAIL] pulseq matlab dir missing: {pulseq_dir}")
            any_fail = True
            continue

        write_automation_json(scen_path, seq_abs, export_dir.resolve())
        rc, so, se = run_cmd([str(exe), "--automation", str(scen_path)], cwd=repo_root)
        if rc != 0:
            print(f"[FAIL] seqeyes automation failed (rc={rc})")
            if so.strip():
                print(so.strip())
            if se.strip():
                print(se.strip())
            any_fail = True
            continue

        cpp_adc_path = export_dir / "ktraj_adc.txt"
        if not cpp_adc_path.exists():
            print(f"[FAIL] missing C++ export: {cpp_adc_path}")
            any_fail = True
            continue

        matlab_script = (case_dir / "run_matlab_compare.m").resolve()
        matlab_adc_path = (case_dir / "ktraj_adc_matlab.txt").resolve()
        write_matlab_script(matlab_script, pulseq_dir, seq_abs, matlab_adc_path)

        rc, so, se = run_cmd(
            [args.matlab_cmd, "-batch", f"run('{matlab_script.as_posix()}');"],
            cwd=repo_root,
        )
        if rc != 0:
            print(f"[FAIL] MATLAB compare failed (rc={rc})")
            if so.strip():
                print(so.strip())
            if se.strip():
                print(se.strip())
            any_fail = True
            continue

        try:
            cpp_adc = normalize_to_nx3(read_matrix(cpp_adc_path))
            matlab_adc = normalize_to_nx3(read_matrix(matlab_adc_path))
            stats = compute_stats(cpp_adc, matlab_adc)
        except Exception as ex:
            print(f"[FAIL] parse/compare error: {ex}")
            any_fail = True
            continue

        ok = pass_threshold(stats, thr)
        print(
            f"rows: cpp={stats['n_cpp']} matlab={stats['n_matlab']} used={stats['n_used']}\n"
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

        if not args.keep_temp:
            # Keep main artifacts (export + matlab_adc), remove script
            if matlab_script.exists():
                matlab_script.unlink()

    if any_fail:
        print("\n[FAILED] Trajectory regression has failures.")
        sys.exit(1)
    print("\n[SUCCESS] Trajectory regression passed.")
    sys.exit(0)


if __name__ == "__main__":
    main()
