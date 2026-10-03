# Supported Definitions

SeqEyes reads selected entries from the Pulseq `[DEFINITIONS]` section to improve visualization and metadata display.

| Definition | Value format | Used for | Usage |
|---|---|---|---|
| `B0` | Tesla | Frequency and phase display calculations that need field strength. | |
| `EchoTime` | Seconds | TE overlay and TE-dependent display logic. | |
| `FOV` | Meters | `1/FOV` k-space unit display and tolerance calculations. | |
| `RepetitionTime` | Seconds | Enables TR-based navigation and display. | |
| `SystemName` | Text | Matches the sequence to a Settings --> Safety system profile whose profile name equals this definition value. |  |
| `TE` | Seconds | Alias for echo time. Used if `EchoTime` is absent. | |
| `TR` | Seconds | Alias for repetition time. Used if `RepetitionTime` is absent. | |
| `TridIdName` | Comma-separated names | Maps numeric `TRID` label values to names in tooltips and extension displays. Index is 1-based. | `seq.setDefinition('TridIdName', strjoin(seq.tridId2Name, ','));` |


- `SystemName` names the scanner the sequence was written for. SeqEyes always checks a sequence against the profile currently selected in Settings > Safety, which provides the safety limits (`maxGrad`, `maxSlew`, `maxB1`), the Siemens ASC file for PNS, and B0 (see below). `SystemName` only offers to switch that selection when the sequence is loaded:

  | Sequence | What happens on load |
  |---|---|
  | No `SystemName`, or it equals the selected profile | Nothing; the selected profile is used. |
  | `SystemName` matches another profile name (case-insensitive) | Depends on Settings > Misc > System Profile > *When SystemName differs* (see below). |
  | `SystemName` matches no profile | The selected profile is used, and a warning is shown. |

  *When SystemName differs* options:

  - **Ask** (default): a dialog offers to switch the selected profile to the matching one or keep the current one. The dialog is not shown again when the same file is reloaded.
  - **Always switch**: the selected profile is switched without asking.
  - **Never switch**: the selected profile is kept without asking.

  Switching changes the selected profile itself, so it stays selected for sequences opened later. For example, with `skyra` selected and a profile named `Prisma` defined, opening a sequence with `SystemName Prisma` and choosing *Switch* makes `Prisma` the selected profile; choosing *Keep* checks the sequence against `skyra`.

- For ppm-based RF/ADC offsets, SeqEyes resolves field strength in this order:

  ```text
  sequence B0 definition > selected system profile B0 > 3.0 T
  ```

  If both the sequence and the selected system profile define different B0 values, SeqEyes uses the sequence `B0` and shows a warning.

- `TridIdName` maps 1-based numeric `TRID` values to display names. For example, if `seq.tridId2Name` is:

  ```matlab
  {'inv_prep_1', 'fat_suppression', 'readout'}
  ```

  then `TRID=3` is displayed as `TRID=3, readout`.

- Standard Pulseq timing definitions such as `GradientRasterTime`, `RadiofrequencyRasterTime`, `AdcRasterTime`, and `BlockDurationRaster` are parsed as part of normal Pulseq loading and are not listed here.
