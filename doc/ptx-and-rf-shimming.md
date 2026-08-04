# pTx and RF Shimming Support

SeqEyes can visualize multi-channel RF waveforms in Pulseq sequences when channel information is available through a supported representation.

## Supported Inputs

SeqEyes currently supports two multi-channel RF sources:

| Source | Format |
|---|---|
| `RF_SHIMS` | Pulseq v1.5.1+ RF shimming extension |
| Roos pTx format | RF/ADC phase-table pattern proposed for pTx Pulseq files |

Normal single-channel RF pulses continue to be displayed as one RF channel.

## Display Behavior

- RF magnitude is shown on the `RF mag` row.
- RF phase is shown on the `RF/ADC ph` row.
- Multi-channel RF pulses are drawn on the same `RF mag` and `RF/ADC ph` axes, with each channel shown in a different color.

## Roos pTx Auto-Detection

SeqEyes includes optional auto-detection for the pTx format proposed by Thomas H. M. Roos, https://onlinelibrary.wiley.com/doi/full/10.1002/mrm.30601 .

Example sequence:

https://github.com/Roosted7/ptx-pulseq/blob/v1.5/matlab/demoSeq/gre2d_pTxSingleChan_8Tx.seq


When detected, SeqEyes splits compatible RF shape data into multiple transmit channels for visualization.

## Conflict Handling

A sequence must not mix explicit `RF_SHIMS` with the detected Roos pTx encoding.

If both are present, SeqEyes treats the file as ambiguous and reports a load error.

## Troubleshooting

If a pTx sequence appears as single-channel RF:

- Check whether the file uses explicit `RF_SHIMS`.
- Enable the Roos pTx option if the file uses that RF/ADC phase-table encoding.
- Inspect the load/status message for the detected channel count.