# SeqEyes
Display Pulseq sequence diagram and k-space trajectory, modified from [PulseqViewer](https://github.com/xpjiang/PulseqViewer)

A brief overview of SeqEyes can be found at the  [2026 pulseq virtual meeting](https://github.com/pulseq/ISMRM-Virtual-Meeting--February-24-26-2026/blob/main/slides/day2_Seqeyes_sequence_and_trajectory_viewer_tool.pdf).

![image](./doc/ui.png)

## Install
- Windows:
  - Download the compiled `.exe` from [github releases](https://github.com/xingwangyong/seqeyes/releases)
  - Install with `pip install seqeyes`.
- Linux: Download the AppImage from [github releases](https://github.com/xingwangyong/seqeyes/releases).
- macOS:
  - Recommended: install with `pip install seqeyes`.
  - Alternatively: download the unsigned app bundle zip from [github releases](https://github.com/xingwangyong/seqeyes/releases). See [macOS unsigned app notes](doc/macos_unsigned_app.md).

## Usage
- Open GUI, load .seq file
- Use the command line interface 
```bash
seqeyes filename.seq
```
for more options, see `seqeyes --help`
- Use the matlab wrapper `seqeyes.m`
```matlab
>>seqeyes('path/to/sequence.seq');
```
or
```matlab
>>seqeyes(seq);
```
- Use the python wrapper, install with `pip install seqeyes` and then:
```python
import seqeyes
seqeyes.seqeyes('path/to/sequence.seq')
```
or
```python
seqeyes.seqeyes(seq)
```

## Supported Definitions

SeqEyes recognizes several optional Pulseq `[DEFINITIONS]` entries for enhanced display, TR navigation, TE overlays, trajectory units, and extension tooltips. See [Supported Definitions](doc/supported_definitions.md).

## Build Instructions
Please see [build.md](build.md) for detailed build steps.

## Known Issues

Please see [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for a list of known issues and limitations.

