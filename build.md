# Build Instructions
Qt6 libraries and cmake are required to build the project.

## Linux
Use the build.sh script to build the project:
```bash
./build.sh . ./out/bin Release ~/Qt
```

## macOS
Recommended dependencies installation via Homebrew:
```bash
brew install qt
brew install cmake
```

Use the same build.sh script (replace `/path/to/Qt` to the installation path of Qt):
```bash
./build.sh . ./out/bin Release /path/to/Qt
```
The script auto-detects the Qt desktop folder (`macos` or `clang_64`) and uses `macdeployqt` through CMake install rules.

If Qt is installed with Homebrew:
```bash
./build.sh . ./out/bin Release "$(brew --prefix qt)"
```

## Windows
```
cmake -S . -B out/build/x64-Release
cmake --build out/build/x64-Release --config Release
```
After compilation, run the following command to deploy Qt libraries:
```bash
C:\Qt\6.5.3\msvc2019_64\bin\windeployqt.exe .\seqeyes.exe
```

**Note**: Please use the full path to run windeployqt.exe, as the system may have multiple versions of Qt installed.
