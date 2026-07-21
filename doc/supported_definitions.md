# Supported Definitions

SeqEyes reads selected entries from the Pulseq `[DEFINITIONS]` section to improve visualization and metadata display.

| Definition | Value format | Used for | Usage |
|---|---|---|---|
| `B0` | Tesla | Frequency and phase display calculations that need field strength. | |
| `EchoTime` | Seconds | TE overlay and TE-dependent display logic. | |
| `FOV` | Meters | `1/FOV` k-space unit display and tolerance calculations. | |
| `RepetitionTime` | Seconds | Enables TR-based navigation and display. | |
| `SystemName` | Text | Matches the sequence to a Settings > Safety system profile by profile name. If `B0` is absent, the matched profile B0 is used for ppm-based RF/ADC frequency and phase calculations. | `SystemName Prisma` |
| `TE` | Seconds | Alias for echo time. Used if `EchoTime` is absent. | |
| `TR` | Seconds | Alias for repetition time. Used if `RepetitionTime` is absent. | |
| `TridIdName` | Comma-separated names | Maps numeric `TRID` label values to names in tooltips and extension displays. Index is 1-based. | `seq.setDefinition('TridIdName', strjoin(seq.tridId2Name, ','));` |

For ppm-based RF/ADC offsets, SeqEyes resolves field strength in this order:

```text
sequence B0 definition > matched/active system profile B0 > 3.0 T
```

If both the sequence and the selected or matched system profile define different B0 values, SeqEyes uses the sequence `B0` and shows a warning.

For `TridIdName`, if `seq.tridId2Name` is:

```matlab
{'inv_prep_1', 'fat_suppression', 'readout'}
```

then `TRID=3` is displayed as `TRID=3, readout`.

Standard Pulseq timing definitions such as `GradientRasterTime`, `RadiofrequencyRasterTime`, `AdcRasterTime`, and `BlockDurationRaster` are parsed as part of normal Pulseq loading and are not listed here.
