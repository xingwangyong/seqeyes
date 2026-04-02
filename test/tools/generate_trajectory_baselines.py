import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
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
    cases = data.get("cases", [])
    if not isinstance(cases, list) or not cases:
        eprint("[ERROR] Cases file has no valid 'cases' list")
        sys.exit(1)

    out = []
    for c in cases:
        cid = str(c.get("id", "")).strip()
        seq = expand_env_vars(str(c.get("seq", "")).strip())
        if not cid or not seq:
            eprint(f"[ERROR] Invalid case entry: {c}")
            sys.exit(1)
        out.append({"id": cid, "seq": seq})
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


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            b = f.read(1024 * 1024)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def line_count(path: Path) -> int:
    with path.open("r", encoding="utf-8", errors="ignore") as f:
        return sum(1 for ln in f if ln.strip())


def git_short_head(repo_root: Path) -> str:
    rc, so, _ = run_cmd(["git", "rev-parse", "--short", "HEAD"], cwd=repo_root)
    return so.strip() if rc == 0 else ""


def main():
    ap = argparse.ArgumentParser(description="Generate trajectory baselines (ktraj + ktraj_adc) by running seqeyes automation.")
    ap.add_argument("--bin-dir", required=True, help="Directory containing seqeyes.exe")
    ap.add_argument("--cases", default="test/trajectory_regression_cases.yaml", help="YAML cases file")
    ap.add_argument("--out-dir", default="test/trajectory_baselines", help="Output baseline directory")
    ap.add_argument("--only", default="", help="Comma-separated case ids to run")
    ap.add_argument("--clean", action="store_true", help="Delete out-dir before generating")
    args = ap.parse_args()

    repo_root = Path.cwd().resolve()
    exe = find_seqeyes_exe(Path(args.bin_dir).resolve())
    cases = load_cases(Path(args.cases).resolve())
    out_root = Path(args.out_dir).resolve()

    only_set = set(x.strip() for x in args.only.split(",") if x.strip())
    if only_set:
        cases = [c for c in cases if c["id"] in only_set]
        if not cases:
            eprint("[ERROR] --only filtered out all cases")
            sys.exit(1)

    if args.clean and out_root.exists():
        shutil.rmtree(out_root)
    out_root.mkdir(parents=True, exist_ok=True)

    print(f"Using seqeyes: {exe}")
    print(f"Cases: {len(cases)}")

    manifest = {
        "generated_at_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git_head_short": git_short_head(repo_root),
        "seqeyes_path": exe.as_posix(),
        "cases": [],
    }

    any_fail = False
    for c in cases:
        cid = c["id"]
        seq_abs = (repo_root / c["seq"]).resolve()
        case_dir = (out_root / cid).resolve()
        case_dir.mkdir(parents=True, exist_ok=True)
        export_dir = (case_dir / "export").resolve()
        export_dir.mkdir(parents=True, exist_ok=True)
        scen_path = (case_dir / "scenario.json").resolve()

        print(f"\n--- [{cid}] ---")
        print(f"seq: {seq_abs}")
        if not seq_abs.exists():
            print(f"[FAIL] missing seq: {seq_abs}")
            any_fail = True
            manifest["cases"].append({"id": cid, "seq": seq_abs.as_posix(), "status": "fail_missing_seq"})
            continue

        write_automation_json(scen_path, seq_abs, export_dir)
        rc, so, se = run_cmd([str(exe), "--automation", str(scen_path)], cwd=repo_root)
        if rc != 0:
            print(f"[FAIL] automation failed (rc={rc})")
            if so.strip():
                print(so.strip())
            if se.strip():
                print(se.strip())
            any_fail = True
            manifest["cases"].append({"id": cid, "seq": seq_abs.as_posix(), "status": "fail_automation", "rc": rc})
            continue

        ktraj = export_dir / "ktraj.txt"
        ktraj_adc = export_dir / "ktraj_adc.txt"
        if not ktraj.exists() or not ktraj_adc.exists():
            print("[FAIL] missing export file(s)")
            any_fail = True
            manifest["cases"].append({"id": cid, "seq": seq_abs.as_posix(), "status": "fail_missing_export"})
            continue

        case_info = {
            "id": cid,
            "seq": seq_abs.as_posix(),
            "status": "ok",
            "files": {
                "ktraj": {
                    "path": ktraj.as_posix(),
                    "sha256": file_sha256(ktraj),
                    "rows": line_count(ktraj),
                },
                "ktraj_adc": {
                    "path": ktraj_adc.as_posix(),
                    "sha256": file_sha256(ktraj_adc),
                    "rows": line_count(ktraj_adc),
                },
            },
        }
        manifest["cases"].append(case_info)
        print(f"[OK] exported rows: ktraj={case_info['files']['ktraj']['rows']} adc={case_info['files']['ktraj_adc']['rows']}")

    manifest_path = out_root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"\nManifest: {manifest_path}")

    if any_fail:
        print("[FAILED] Some baseline exports failed.")
        sys.exit(1)
    print("[SUCCESS] Trajectory baselines generated.")
    sys.exit(0)


if __name__ == "__main__":
    main()
