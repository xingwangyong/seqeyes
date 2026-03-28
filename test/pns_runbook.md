# PNS Local Runbook

This runbook is for local validation only (no cloud MATLAB CI).

## Prerequisites
- Built binaries available under `out/build/x64-Release`.
- MATLAB installed and callable from terminal (`matlab -batch`).
- A valid Siemens ASC profile with complete g-scale fields
  (prefer `*_twoFilesCombined.asc`).

## Local Paths (do not commit)
```
ASC:         C:\Users\76494\Syncbb\others_toolboxes\pulseq\matlab\idea\asc\MP_GPA_K2309_2250V_951A_AS82_XA30A_mod_twoFilesCombined.asc
MATLAB pulseq: C:\Users\76494\Syncbb\others_toolboxes\pulseq_matlab_diff_versions\pulseq_v151_xy\matlab
```

## 1) Build
```powershell
cmake --build out/build/x64-Release --config Release --target PnsDumpTest seqeyes
```

Expected:
- Build succeeds without errors.
- `out/build/x64-Release/test/Release/PnsDumpTest.exe` exists.

## 2) MATLAB parity check (three sequences, threshold 1e-4)

Set variables:
```powershell
$asc = "C:\Users\76494\Syncbb\others_toolboxes\pulseq\matlab\idea\asc\MP_GPA_K2309_2250V_951A_AS82_XA30A_mod_twoFilesCombined.asc"
$md  = "C:\Users\76494\Syncbb\others_toolboxes\pulseq_matlab_diff_versions\pulseq_v151_xy\matlab"
$bin = "out/build/x64-Release/test/Release"
```

Run:
```powershell
python test/tools/compare_pns_with_matlab.py `
  --bin-dir $bin `
  --seq test/seq_files/writeGradientEcho.seq `
  --asc $asc --pulseq-matlab-dir $md `
  --out-dir test/pns_compare_gre `
  --sample-stride 20 --max-abs-threshold 1e-4

python test/tools/compare_pns_with_matlab.py `
  --bin-dir $bin `
  --seq test/seq_files/writeGradientEcho_label.seq `
  --asc $asc --pulseq-matlab-dir $md `
  --out-dir test/pns_compare_writeGradientEcho_label `
  --sample-stride 20 --max-abs-threshold 1e-4

python test/tools/compare_pns_with_matlab.py `
  --bin-dir $bin `
  --seq test/seq_files/writeSpiral.seq `
  --asc $asc --pulseq-matlab-dir $md `
  --out-dir test/pns_compare_spi `
  --sample-stride 20 --max-abs-threshold 1e-4
```

Expected:
- All three exit with `[PASS]`.
- `writeGradientEcho.seq`: `x/y/z/norm max_abs <= 1e-4`.
- `writeGradientEcho_label.seq`: `x/y/z/norm max_abs <= 1e-4`.
- `writeSpiral.seq`: `x/y/z/norm max_abs <= 1e-4`.

## 3) Local worst-case PNS performance check
```powershell
python test/test_perf_zoom.py `
  --bin-dir out/build/x64-Release/Release `
  --seq test/seq_files/writeGradientEcho_label.seq `
  --repeat 5 --warmup `
  --use-dummy-pns-asc
```

Expected:
- Script prints `ZOOM_MS` and exits `[OK]`.
- No crash/hang in automation run.

## 4) Quick troubleshooting
- If ASC parse fails (missing g-scale), switch to a complete ASC file (`*_twoFilesCombined.asc`).
- If MATLAB parity fails to start, verify `--pulseq-matlab-dir` points to folder containing `+mr`.
- If `PnsDumpTest.exe` fails with DLL issues, ensure Qt runtime bin path is in `PATH`.
