# PNS Local Runbook

This runbook is for local validation only (no cloud MATLAB CI).

## Prerequisites
- Built binaries available under `out/build/x64-Release`.
- MATLAB installed and callable from terminal (`matlab -batch`).
- A valid Siemens ASC profile with complete g-scale fields
  (prefer `*_twoFilesCombined.asc`).

## 1) Build
```powershell
cmake --build out/build/x64-Release --config Release --target PnsDumpTest seqeyes
```

Expected:
- Build succeeds without errors.
- `out/build/x64-Release/test/Release/PnsDumpTest.exe` exists.

## 2) MATLAB parity check
Example command:
```powershell
python test/tools/compare_pns_with_matlab.py `
  --bin-dir out/build/x64-Release/test/Release `
  --seq test/seq_files/writeGradientEcho.seq `
  --asc "C:/.../MP_GPA_K2309_2250V_951A_AS82_XA30A_mod_twoFilesCombined.asc" `
  --pulseq-matlab-dir "C:/.../pulseq_v151/matlab" `
  --sample-stride 20 `
  --max-abs-threshold 0.05
```

Expected:
- Script exits with `[PASS]`.
- `writeGradientEcho.seq`: max abs should be near machine precision (very close to 0).
- `spi_sub.seq`: currently acceptable at `max_abs <= 0.05` (known residual for x/y/norm).

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
