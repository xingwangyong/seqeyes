# Custom Pulseq LABEL Extensions

SeqEyes supports visualization of user-defined Pulseq LABEL.

## MATLAB Example

A custom label can be registered before it is used in a sequence:

```matlab
mr.addCustomLabel('my_own_label');
```

The label can then be attached to a sequence block:

```matlab
seq.addBlock(mr.makeLabel('SET', 'my_own_label', 123));
```

When the resulting `.seq` file is opened in SeqEyes, `my_own_label` is shown together with other Pulseq LABEL values on the `ADC/labels` row and in the extension label tooltip when enabled.