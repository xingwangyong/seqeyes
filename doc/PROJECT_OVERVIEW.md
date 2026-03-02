# SeqEyes — Pulseq Sequence Viewer (Qt + QCustomPlot)

SeqEyes is a fast, interactive, cross‑platform viewer for Pulseq `.seq` files. It focuses on real‑time interaction, responsiveness, and a workflow independent of MATLAB/Python. It is ideal for quick sequence verification, debugging, and comparison in research and development.

The application parses Pulseq sequences, builds merged time series for RF/gradients/ADC, and renders them on synchronized axes.

## Core Goals

- High‑performance waveform and 2D trajectory rendering with smooth pan/zoom
- Clear and stable visualization of sequence structure (TRs, blocks, labels)
- Intuitive and robust interactions (mouse wheel, drag, context menu)
- Deterministic axis behavior to avoid jitter/flicker on TR switching
- Practical testing infrastructure (C++ QtTest + Python runners)

## High‑Level Architecture

- Qt Widgets (C++/Qt6): application framework and UI
- QCustomPlot: multi‑axis rendering surface and plots
- Custom modules:
  - PulseqLoader: parse `.seq`, build merged series, manage time units
  - WaveformDrawer: axis/plot creation and drawing, LOD/downsampling, y‑axis range policy
  - TRManager: TR/time controls, dual range sliders, render modes, state sync
  - InteractionHandler: input handling (wheel/drag), axis synchronization, bounds
  - ZoomManager: LOD thresholds and downsampling settings
  - Settings/SettingsDialog: units, visibility, UI configuration
  - SeriesBuilder: merged time series (RF/gradients/ADC)
  - DoubleRangeSlider: custom dual‑handle slider (for TR/time ranges)

External dependencies:
- `src/external/pulseq`: Pulseq sequence loader (supports v1.5.1)
- `src/external/qcustomplot`: QCustomPlot sources

## Data Flow

- Loading `.seq` (PulseqLoader):
  - Read version information and construct version‑aware loader
  - Parse blocks and build block edges (internal time: μs × tFactor)
  - Build merged/fused series:
    - RF magnitude/phase: `getRfTimeAmp/getRfAmp`, `getRfTimePh/getRfPh`
    - Gradients: `getGxTime/getGxValues`, `getGyTime/getGyValues`, `getGzTime/getGzValues`
    - ADC: `getAdcTime/getAdcValues`
  - Detect TRs and compute TR block indices
  - Initialize block ranges and notify TRManager to set up controls

- Initial rendering (WaveformDrawer):
  - Create six stacked axis rects: ADC/labels, RF magnitude, RF/ADC phase, Gx, Gy, Gz
  - Configure a shared bottom axis and synchronized horizontal ranges
  - Draw curves for the initial viewport; optionally draw block edges

- Units:
  - Internal time is μs × tFactor (e.g., ms → tFactor = 1e‑3). Conversion is centralized to keep axes consistent.

## Rendering Pipeline

- Axis layout:
  - Six `QCPAxisRect`s stacked vertically; only the bottom rect shows the x‑axis labels
  - Left axes show ADC/RF/gradients with labels and consistent margins

- Merged series and slicing:
  - Drawing methods slice merged series to the current viewport with small margins
  - NaN separators preserve segment boundaries (gaps) inside series

- LOD / downsampling:
  - LTTB downsampling with an explicit “target points (pixel budget)” interface
  - API: `WaveformDrawer::applyLTTBDownsampling(time, values, targetPoints, outTime, outValues)`; targetPoints is guarded (≤ segment size, at least 2)
  - Currently all call sites use default `targetPoints=1000` for consistent LOD; a viewport‑aware budget (derived from width/device pixels) can be integrated later
  - QCustomPlot adaptive sampling is disabled; downsampling is explicit and reproducible
 
- Y‑axis range policy (stable, flicker‑free):
  - On load, SeqEyes computes global min/max per channel across the whole sequence and locks y‑ranges for all rects
  - Locked y‑axes prevent per‑window autoscale jitter when switching TRs or small pan/zoom changes
  - API: `WaveformDrawer::computeAndLockYAxisRanges()` (called after merged series are built)

- Repaint strategy:
  - Axis synchronization goes through a single path to avoid redundant rangeChanged cascades
  - During TR range changes, intermediate updates are batched so only one visible replot occurs

## Interaction Model

- Wheel pan/zoom (InteractionHandler):
  - Handle real mouse events from QCustomPlot signals
  - Coalesce wheel events (≈70 Hz) to avoid “zoom burst after wheel stop”
  - Mouse‑anchored zoom (Adobe‑style); ~10% per wheel step with an exponential factor
  - Valid pan/zoom bounds:
    - TR mode: absolute [startTR, endTR]
    - Whole‑sequence: [0, end of data]

- Axis synchronization:
  - Horizonal range is set on all rects with signals blocked during updates, then a single replot

- Other tools:
  - Context menu: zoom in/out, info dialog
  - Axis label drag‑reorder with drag ghost and drop indicator
  - Δt measurement mode with markers and shading

## TR / Time Controls

- Dual sliders (DoubleRangeSlider):
  - TR range slider (index‑based, 1‑based in UI)
  - Time range slider (ms):
    - TR mode: values relative to the selected TR span
    - Whole‑sequence: values are absolute over the whole sequence

- Loop prevention & one‑way sync:
  - Programmatic updates to sliders and inputs are wrapped in `QSignalBlocker` to prevent feedback loops
  - After pan/zoom, one‑way axis→controls sync reflects the current view without emitting signals

- Window persistence across TR switches:
  - Preserve the last relative window (start/end ms) on TR changes; reuse the same relative window (clamped to TR length) in the next TR

## Performance Considerations

- Rendering:
  - Merged series, explicit per‑viewport slicing, optional LTTB downsampling
  - Locked y‑axis ranges reduce dynamic recalculation and improve stability

- Interaction:
  - Coalesced wheel events provide a “sticky” feel and eliminate over‑zoom after wheel stop

- Minimal replots:
  - When TR range changes, plots are rebuilt with updates disabled, then a single replot after setting ranges

## Testing Infrastructure

- C++ QtTest (headless): `test/TimeSliderSyncTest.cpp`
  - Simulate real user interactions (Ctrl+wheel zoom, wheel pan)
  - Verify sliders silently reflect the viewport (no valuesChanged emissions)
  - Validate pan/zoom bounds and relative window persistence across TR switches

- Python runners (build‑dir aware):
  - `test/test_zoom_pan.py`: run QtTest for all `.seq` files under `test/seq_files`
  - `test/test_load_all.py`: run the app headlessly to load all `.seq` and report PASS/FAIL
  - `test/tools/run_all.py`: small menu to run load/zoompan/both; supports `--bin-dir` pointing to the build output

## Build & Run

- Requirements:
  - Qt6 Core/Gui/Widgets/PrintSupport/Test
  - CMake (presets provided), C++17 compiler

- Build:
  - `cmake -S . -B out/build -G "Visual Studio 17 2022"`
  - `cmake --build out/build --config Release`

- Run:
  - GUI: `SeqEyes [file.seq]` (options in `src/main.cpp`)
  - Headless load check: `SeqEyes --headless --exit-after-load file.seq`

- Tests:
  - `python test/test_zoom_pan.py --bin-dir out/build/x64-Debug`
  - `python test/test_load_all.py --bin-dir out/build/x64-Debug`
  - `python test/tools/run_all.py --bin-dir out/build/x64-Debug`

- Version info:
  - Auto‑generated from Git in the format `YYYYMMDD, git commit hash` (e.g., `20241215, a1b2c3d4e5`), using the commit’s date; shown in Help → About and `--version`.

## Key Design Decisions

- Locked y‑axis ranges by default
  - Improve stability when comparing TRs and during pan/zoom
  - A future toggle can enable dynamic autoscale for exploratory tasks

- One‑way axis→controls synchronization
  - Prevent UI update loops; ensure sliders reflect the view without driving more changes

- Coalesced wheel handling
  - Process multiple small wheel increments together at ~70 Hz to eliminate “post‑scroll zooming”

## Module Reference

- MainWindow (`src/mainwindow.*`)
  - App entry, menus/actions, status bar, connecting handlers
  
- PulseqLoader (`src/PulseqLoader.*`)
  - Load/parse `.seq`, manage definitions, build merged series, compute TRs
  - Provide merged time/value arrays and block edges
  
- WaveformDrawer (`src/WaveformDrawer.*`)
  - Create and manage axis rects and plots
  - Draw RF/gradients/ADC curves from merged series with slicing and LOD
  - Compute and lock y‑axis ranges for a jitter‑free visual
  - LTTB downsampling API: `applyLTTBDownsampling(..., int targetPoints, ...)`; per segment (NaN‑delimited); default 1000
  
- TRManager (`src/TRManager.*`)
  - TR/time UI (dual slider + line edits), render modes, visibility toggles
  - Preserve relative window across TR changes; synchronize axes; debounced updates
  
- InteractionHandler (`src/InteractionHandler.*`)
  - Wheel pan/zoom, axis sync, drag‑reorder, context menu, Δt measurement
  - Bounds for TR vs whole sequence; wheel coalescing and mouse‑anchored zoom
  
- ZoomManager (`src/ZoomManager.*`)
  - LOD thresholds and configuration (downsampling factors)
  - Plan: provide a viewport‑aware pixel budget to drive `targetPoints` (for now default 1000)
  
- Settings / SettingsDialog (`src/Settings*.{h,cpp}`)
  - Persist UI settings, units, and curve visibility presets
  
- SeriesBuilder (`src/SeriesBuilder.*`)
  - Build merged curves for RF/gradients/ADC from decoded blocks and edges
  
- DoubleRangeSlider (`src/doublerangeslider.*`)
  - Custom dual‑handle slider used by TR/time range controls

## Roadmap / Ideas

- Persistent graphs (no clear/rebuild)
  - Replace “clearGraphs + addGraph” with persistent `QCPGraph` objects per channel and `setData()` updates to further eliminate flicker on TR switches
  
- UI setting: dynamic vs locked y‑axis
  - Add a user toggle (default locked) to enable autoscale when desired
  
- Finer‑grained tests
  - Add tests for axis reordering, measurement mode, and LOD transitions
  
- Performance profiling & caching
  - Extend LOD cache or precompute downsampled variants for frequently viewed ranges

## Top‑Level Tree

- `src/` — application sources (UI, loader, rendering, interactions)
- `test/` — Qt tests + Python runners + sample `.seq`
- `docs/` — documentation assets
- `CMakeLists.txt` + presets — build configuration
