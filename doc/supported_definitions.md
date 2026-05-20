# Supported Definitions

SeqEyes reads selected entries from the Pulseq `[DEFINITIONS]` section to improve visualization and metadata display.

| Definition | Value format | Used for | Usage |
|---|---|---|---|
| `B0` | Tesla | Frequency and phase display calculations that need field strength. | |
| `EchoTime` | Seconds | TE overlay and TE-dependent display logic. | |
| `FOV` | Meters | `1/FOV` k-space unit display and tolerance calculations. | |
| `RepetitionTime` | Seconds | Enables TR-based navigation and display. | |
| `TE` | Seconds | Alias for echo time. Used if `EchoTime` is absent. | |
| `TR` | Seconds | Alias for repetition time. Used if `RepetitionTime` is absent. | |
| `TridIdName` | Comma-separated names | Maps numeric `TRID` label values to names in tooltips and extension displays. Index is 1-based. | `seq.setDefinition('TridIdName', strjoin(seq.tridId2Name, ','));` |

For `TridIdName`, if `seq.tridId2Name` is:

```matlab
{'inv_prep_1', 'fat_suppression', 'readout'}
```

then `TRID=3` is displayed as `TRID=3, readout`.

Standard Pulseq timing definitions such as `GradientRasterTime`, `RadiofrequencyRasterTime`, `AdcRasterTime`, and `BlockDurationRaster` are parsed as part of normal Pulseq loading and are not listed here.
