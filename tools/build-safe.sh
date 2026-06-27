#!/usr/bin/env bash
# =============================================================================
# build-safe.sh — Safe-mode build for seqeyes on memory-constrained VMs.
# =============================================================================
#
# Background:
#   The official ./build.sh defaults to -j$(nproc) parallel compilation, which
#   assumes the build host has enough RAM for N concurrent cc1plus processes.
#   On a small VM (1.6 GB RAM, 1 vCPU) this fails because Qt 6 + QCustomPlot
#   translation units can hit ~1.3 GB peak RSS each during compilation.
#
# What this script does differently (validated on 1 vCPU / 1.6 GB RAM / 2 GB swap):
#   1. Ensures a swapfile is enabled (else build OOMs at ~10% progress)
#   2. Bumps vm.swappiness from default 0 to 100 (kernel otherwise refuses to
#      swap out cold pages, so cc1plus gets OOM-killed even with swap present)
#   3. Skips the C++ unit tests by default (-DSEQEYES_BUILD_TESTS=OFF); they
#      add 3 more heavy targets that push peak memory beyond 1.6 GB
#   4. Forces single-threaded compilation (-j 1)
#   5. Forces system gcc/g++ (avoids conda-forge sysroot mismatch)
#
# Usage:
#   ./tools/build-safe.sh                          # default settings
#   ./tools/build-safe.sh /path/to/Qt6/prefix      # custom Qt6 prefix
#   SEQEYES_BUILD_TESTS=ON ./tools/build-safe.sh   # also build tests (needs ~3 GB RAM)
#
# Env vars:
#   BUILD_TYPE              Release | Debug           (default: Release)
#   SEQEYES_BUILD_TESTS     ON | OFF                  (default: OFF)
#   SWAPFILE                path to swapfile          (default: /swapfile)
#   JOBS                    parallel jobs             (default: 1; risky to raise)
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_TYPE="${BUILD_TYPE:-Release}"
QT_PREFIX="${1:-/opt/miniforge3/envs/seqeyes}"
SWAPFILE="${SWAPFILE:-/swapfile}"
JOBS="${JOBS:-1}"
BUILD_TESTS_FLAG="${SEQEYES_BUILD_TESTS:-OFF}"

# ---- conda env activation ---------------------------------------------------
# Toolchain (cmake, ninja) lives inside the micromamba env, not on the default
# PATH. Prepend it explicitly so this script works whether or not the caller
# has already activated the env.
ENV_BIN="${QT_PREFIX}/bin"
if [[ -d "${ENV_BIN}" ]]; then
  export PATH="${ENV_BIN}:${PATH}"
fi

# ---- pretty output ----------------------------------------------------------
if [[ -t 1 ]]; then
  C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_RST=$'\033[0m'
else
  C_OK=; C_WARN=; C_ERR=; C_RST=
fi
step() { printf "\n${C_OK}==>${C_RST} %s\n" "$*"; }
warn() { printf "${C_WARN}!!${C_RST} %s\n" "$*"; }
die()  { printf "${C_ERR}XX${C_RST} %s\n" "$*" >&2; exit 1; }

# ---- preflight --------------------------------------------------------------
[[ -f "${PROJECT_ROOT}/CMakeLists.txt" ]] || die "CMakeLists.txt not found in ${PROJECT_ROOT}"

# ---- 1. swap ----------------------------------------------------------------
step "Step 1/5: Ensure swap is available"
if swapon --show=NAME --noheadings 2>/dev/null | grep -q .; then
  echo "Active swap:"
  swapon --show
else
  warn "No active swap."
  if [[ -f "${SWAPFILE}" ]]; then
    warn "Found ${SWAPFILE}, enabling..."
    swapon "${SWAPFILE}" || die "swapon failed; check permissions and that file is mkswap'd"
  else
    warn "No swapfile at ${SWAPFILE}. Recommend creating 2 GB swap:"
    warn "  fallocate -l 2G ${SWAPFILE} && chmod 600 ${SWAPFILE} && \\"
    warn "    mkswap ${SWAPFILE} && swapon ${SWAPFILE}"
    warn "Continuing without swap — build will likely OOM."
  fi
fi

# ---- 2. swappiness ----------------------------------------------------------
step "Step 2/5: Set vm.swappiness=100"
echo "Current: $(cat /proc/sys/vm/swappiness)"
if [[ -w /proc/sys/vm/swappiness ]]; then
  echo 100 > /proc/sys/vm/swappiness
  echo "New:     $(cat /proc/sys/vm/swappiness)"
else
  warn "Cannot write /proc/sys/vm/swappiness (need CAP_SYSCTL / root)"
  warn "Without this, kernel may refuse to swap and OOM-kill cc1plus."
fi

# ---- 3. fresh build dir -----------------------------------------------------
step "Step 3/5: Fresh build directory"
if [[ -d "${PROJECT_ROOT}/build_release" ]]; then
  TS=$(date +%s)
  STASH="${PROJECT_ROOT}/build_release.bak.${TS}"
  mv "${PROJECT_ROOT}/build_release" "${STASH}"
  echo "Previous build moved aside -> $(basename "${STASH}")"
fi

# ---- 4. cmake configure -----------------------------------------------------
step "Step 4/5: cmake configure (BUILD_TYPE=${BUILD_TYPE}, TESTS=${BUILD_TESTS_FLAG}, JOBS=${JOBS})"
mkdir -p "${PROJECT_ROOT}/out/bin"
cmake -S "${PROJECT_ROOT}" -B "${PROJECT_ROOT}/build_release" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_PREFIX_PATH="${QT_PREFIX}" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="${PROJECT_ROOT}/out/bin" \
  -DSEQEYES_BUILD_TESTS="${BUILD_TESTS_FLAG}"

# ---- 5. build ---------------------------------------------------------------
step "Step 5/5: cmake build -j ${JOBS} -v"
cmake --build "${PROJECT_ROOT}/build_release" -j "${JOBS}" -v

# ---- summary ----------------------------------------------------------------
step "Done"
BIN="${PROJECT_ROOT}/out/bin/seqeyes"
if [[ -x "${BIN}" ]]; then
  printf "Binary: %s (%s)\n" "${BIN}" "$(du -h "${BIN}" | cut -f1)"
  printf "Run:    %s --help\n" "${BIN}"
else
  die "Build reported success but ${BIN} not found — investigate."
fi