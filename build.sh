#!/bin/bash
#
# Cross-platform Qt CMake Build Script (Linux/macOS)
#
# Usage:
# ./build.sh <source_dir> <output_dir> <build_type> <qt_install_root> [optional: temp_build_dir]
#
# Example:
# ./build.sh . ./out/bin Release ~/Qt

# -------------------------------------------------------------------

# Exit immediately if any command fails
set -e

resolve_existing_dir() {
    (cd "$1" && pwd -P)
}

resolve_maybe_new_path() {
    local input="$1"
    local parent
    local name
    parent="$(dirname "$input")"
    name="$(basename "$input")"
    mkdir -p "$parent"
    echo "$(cd "$parent" && pwd -P)/$name"
}

detect_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
        return
    fi
    if [ "$(uname -s)" = "Darwin" ]; then
        sysctl -n hw.ncpu
        return
    fi
    echo 4
}

to_lower() {
    echo "$1" | tr '[:upper:]' '[:lower:]'
}

detect_qt_prefix_path() {
    local root="$1"
    local os_name="$2"
    local candidate

    # If user passed an executable like qmake/qt-cmake/cmake, infer prefix.
    if [ -f "${root}" ] && [ -x "${root}" ]; then
        candidate="$(cd "$(dirname "${root}")/.." && pwd -P)"
        if [ -d "${candidate}/lib/cmake" ]; then
            echo "${candidate}"
            return
        fi
    fi

    # If user passed a direct Qt prefix (contains lib/cmake), use it as-is.
    if [ -d "${root}/lib/cmake" ]; then
        echo "${root}"
        return
    fi

    # If user passed a Qt version folder (contains macos/gcc_64/clang_64), use it.
    for sub in macos clang_64 gcc_64; do
        if [ -d "${root}/${sub}/lib/cmake" ]; then
            echo "${root}/${sub}"
            return
        fi
    done

    # Otherwise, scan Qt6 installs under root.
    local preferred
    if [ "${os_name}" = "Darwin" ]; then
        preferred="macos clang_64"
    else
        preferred="gcc_64"
    fi

    # Common package-manager style layouts (e.g. Homebrew Cellar path).
    if [ -d "${root}/Cellar/qt" ]; then
        local cellar_qt
        for cellar_qt in "${root}"/Cellar/qt/*; do
            [ -d "${cellar_qt}/lib/cmake" ] || continue
            echo "${cellar_qt}"
            return
        done
    fi

    local version_dir
    local sub
    for version_dir in "${root}"/6.*; do
        [ -d "${version_dir}" ] || continue
        for sub in ${preferred}; do
            if [ -d "${version_dir}/${sub}/lib/cmake" ]; then
                echo "${version_dir}/${sub}"
                return
            fi
        done
    done
}

# --- 1. Help and Argument Check ---
if [ "$#" -lt 4 ]; then
    echo "Usage: $0 <source_dir> <output_dir> <build_type (Debug|Release)> <qt_install_root> [optional: temp_build_dir]"
    echo "Example: $0 ~/hello_qt_cmake ~/my_bin Release ~/qt_inst_root"
    exit 1
fi

# --- 2. Set Variables ---
# readlink -f converts relative paths to absolute paths, making the script more robust
SRC_DIR=$(resolve_existing_dir "$1")
OUTPUT_DIR=$(resolve_maybe_new_path "$2")
BUILD_TYPE=$3
# Qt install root (REQUIRED)
QT_INSTALL_ROOT=$(resolve_existing_dir "$4")

# If the 5th argument (temp build dir) is not specified, use a default
if [ -z "$5" ]; then
    # Use POSIX-compatible lowercase conversion for macOS default Bash 3.2.
    TEMP_BUILD_DIR="${SRC_DIR}/build_$(to_lower "${BUILD_TYPE}")"
    echo "No temp build directory specified, using default: ${TEMP_BUILD_DIR}"
else
    TEMP_BUILD_DIR=$(resolve_maybe_new_path "$5")
fi

# --- 3. Set Your Qt Path (from required argument) ---
# We define the installation root from input, derive prefix path by OS.
UNAME_S=$(uname -s)
QT_PREFIX_PATH="$(detect_qt_prefix_path "${QT_INSTALL_ROOT}" "${UNAME_S}")"

if [ -z "${QT_PREFIX_PATH}" ]; then
    echo "Error: Could not auto-detect a Qt6 desktop prefix under: ${QT_INSTALL_ROOT}"
    echo "Pass Qt root (e.g. ~/Qt), a direct prefix (e.g. ~/Qt/6.8.0/macos),"
    echo "or Homebrew's prefix (e.g. \"\$(brew --prefix qt)\")."
    exit 1
fi

# --- 3b. Set CMake Command ---
# Prefer the CMake bundled with Qt Installer to avoid version conflicts.
# Fall back to system cmake if Qt's bundled cmake is not present.
CMAKE_CMD=""
for candidate in \
    "${QT_INSTALL_ROOT}/Tools/CMake/bin/cmake" \
    "${QT_INSTALL_ROOT}/Tools/CMake/CMake.app/Contents/bin/cmake"
do
    if [ -x "${candidate}" ]; then
        CMAKE_CMD="${candidate}"
        break
    fi
done
if [ -z "${CMAKE_CMD}" ]; then
    CMAKE_CMD="$(command -v cmake || true)"
fi
if [ -z "${CMAKE_CMD}" ]; then
    for candidate in /opt/homebrew/bin/cmake /usr/local/bin/cmake; do
        if [ -x "${candidate}" ]; then
            CMAKE_CMD="${candidate}"
            break
        fi
    done
fi

# --- 4. Check and Create Directories ---
echo "--- Preparing Directories ---"
if [ ! -f "${SRC_DIR}/CMakeLists.txt" ]; then
    echo "Error: CMakeLists.txt not found in source directory: ${SRC_DIR}"
    exit 1
fi

if [ ! -d "${QT_PREFIX_PATH}" ]; then
    echo "Error: Qt prefix path not found: ${QT_PREFIX_PATH}"
    echo "Expected a Qt6 desktop prefix like <QtRoot>/6.x.y/macos, clang_64, or gcc_64"
    exit 1
fi

# Also check if the bundled CMake exists
if [ -z "${CMAKE_CMD}" ] || [ ! -x "${CMAKE_CMD}" ]; then
    echo "Error: CMake executable not found."
    if [ "${UNAME_S}" = "Darwin" ]; then
        echo "macOS: install cmake (e.g. 'brew install cmake') or provide Qt Installer CMake under <QtRoot>/Tools/CMake/."
        echo "Then ensure cmake is in PATH, or use a shell where Homebrew PATH is loaded."
    elif [ "${UNAME_S}" = "Linux" ]; then
        echo "Linux: install cmake (version 3.20+) via your package manager or Qt Installer Tools/CMake."
    else
        echo "Windows: install CMake from Qt Installer ('Developer and Designer Tools' > 'CMake') or system CMake and add it to PATH."
    fi
    exit 1
fi

# Clean up and create directories
rm -rf "${TEMP_BUILD_DIR}"
mkdir -p "${TEMP_BUILD_DIR}"
mkdir -p "${OUTPUT_DIR}"

echo "Source Dir:   ${SRC_DIR}"
echo "Output Dir:     ${OUTPUT_DIR}"
echo "Temp Build Dir: ${TEMP_BUILD_DIR}"
echo "Qt Install Root: ${QT_INSTALL_ROOT}"
echo "Qt Path:      ${QT_PREFIX_PATH}"
echo "CMake Cmd:    ${CMAKE_CMD}"

# Linux toolchain guard:
# Conda compiler wrappers can inject an incompatible sysroot (seen as glibc header
# macro errors like '__BEGIN_NAMESPACE_STD' and bits/mathdef.h failures). By default,
# use system gcc/g++ for C++ builds while still allowing Qt from Conda prefix.
CMAKE_EXTRA_ARGS=()
if [ "${UNAME_S}" = "Linux" ]; then
    if [ -n "${CONDA_PREFIX}" ] && [ "${SEQEYES_ALLOW_CONDA_TOOLCHAIN:-0}" != "1" ]; then
        echo "Detected active Conda environment: ${CONDA_PREFIX}"
        echo "Sanitizing compiler env vars to avoid Conda sysroot/toolchain mismatch."
        unset CC CXX CPPFLAGS CFLAGS CXXFLAGS LDFLAGS CONDA_BUILD_SYSROOT
    fi

    if [ "${SEQEYES_USE_SYSTEM_GCC:-1}" = "1" ] && [ -x /usr/bin/gcc ] && [ -x /usr/bin/g++ ]; then
        CMAKE_EXTRA_ARGS+=(
            -DCMAKE_C_COMPILER=/usr/bin/gcc
            -DCMAKE_CXX_COMPILER=/usr/bin/g++
        )
        echo "Linux: forcing system toolchain: /usr/bin/gcc and /usr/bin/g++"
        echo "Set SEQEYES_USE_SYSTEM_GCC=0 to disable this behavior."
    fi
fi

# --- 5. Run CMake Configure ---
echo "--- Configuring CMake (${BUILD_TYPE} mode)... ---"

# -S (Source Dir), -B (Build Dir)
# CMAKE_RUNTIME_OUTPUT_DIRECTORY is the key to a 'clean' output dir
# We now call the specific $CMAKE_CMD
"${CMAKE_CMD}" -S "${SRC_DIR}" \
      -B "${TEMP_BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DCMAKE_PREFIX_PATH="${QT_PREFIX_PATH}" \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="${OUTPUT_DIR}" \
    "${CMAKE_EXTRA_ARGS[@]}"

# --- 6. Run Make (Build) ---
echo "--- Building... ---"
# -j $(nproc) uses all available CPU cores for parallel compilation
# (Note: 'nproc' might not be available on all systems, replace with -j4, -j8, etc.)
"${CMAKE_CMD}" --build "${TEMP_BUILD_DIR}" --parallel "$(detect_jobs)"

# --- 7. Done ---
echo "--- Build successful! ---"
echo "Final executable is in the (clean) output directory:"
ls -l "${OUTPUT_DIR}"
echo ""
echo "All temporary build files (.o, Makefile) are in: ${TEMP_BUILD_DIR}"