# SeqEyes
Display Pulseq sequence diagram and k-space trajectory, modified from [PulseqViewer](https://github.com/xpjiang/PulseqViewer)

A brief overview of SeqEyes can be found at the  [2026 pulseq virtual meeting](https://github.com/pulseq/ISMRM-Virtual-Meeting--February-24-26-2026/blob/main/slides/day2_Seqeyes_sequence_and_trajectory_viewer_tool.pdf).

![image](./doc/ui.png)

## Install
Download the compiled windows exe from [github releases](https://github.com/xingwangyong/seqeyes/releases), or from [artifacts in github actions](https://github.com/xingwangyong/seqeyes/actions). The latter is more frequently updated.

## Usage
- Open GUI, load .seq file
- Use the command line interface 
```bash
seqeyes filename.seq
```
for more options, see `seqeyes --help`
- Use the matlab wrapper `seqeyes.m`
```matlab
seqeyes('path/to/sequence.seq');
```
or
```matlab
seqeyes(seq);
```

## Build Instructions
Qt6 libraries and cmake are required to build the project.
### Linux
Use the build.sh script to build the project.
### Windows
```
cmake -S . -B out/build/x64-Release
cmake --build out/build/x64-Release --config Release
```
After compilation, run the following command to deploy Qt libraries:
```bash
C:\Qt\6.5.3\msvc2019_64\bin\windeployqt.exe .\seqeyes.exe
```

**Note**: Please use the full path to run windeployqt.exe, as the system may have multiple versions of Qt installed.

## Known Issues

Please see [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for a list of known issues and limitations.



