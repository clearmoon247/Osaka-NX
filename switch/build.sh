#!/usr/bin/env bash
# Builds switch/build/osaka-nx.nro.
# Order: switch/setup_deps.sh, then switch/codegen.sh (needs your ROM), then this (or just run switch/all.sh).
#   - If devkitPro is installed locally (DEVKITPRO set, switch-dev + switch-portlibs), it builds directly.
#   - Otherwise it uses the official devkitpro/devkita64 Docker image (Docker Desktop on Windows works from WSL2).
# PSR_CCACHE=1 caches compiled files for later rebuilds (local devkitPro only; see below).
# PSR_JOBS=N limits parallel compiles (the generated funcs_*.c are huge; too many at once gets cc1 OOM-killed).
set -euo pipefail
cd "$(dirname "$0")/.."

[ -f lib/N64ModernRuntime/librecomp/CMakeLists.txt ] || { echo "run switch/setup_deps.sh first" >&2; exit 1; }
[ -f generated/lookup.cpp ] || { echo "generated/ missing: run switch/codegen.sh first" >&2; exit 1; }
[ -f rsp/aspMain.cpp ] && [ -f rsp/gbTowerMain.cpp ] && [ -f rsp/gbTowerColorMain.cpp ] && [ -f rsp/njpgdspMain.cpp ] || {
    echo "rsp/*.cpp missing: run switch/codegen.sh <your ROM> (it makes them from your ROM; they are not stored in the repository)" >&2; exit 1; }

# Parallel compiles: PSR_JOBS if set, otherwise from the free memory (same rule as build-nro.sh: 2.5 GB reserved for the one big file
# that compiles at a time, then 1.2 GB per compile with PSR_SMALL_CODE=OS or 1.8 GB without), capped at the processor count.
if [ -z "${PSR_JOBS:-}" ] && [ -r /proc/meminfo ]; then
    mem_mb=$(awk '/MemAvailable/ {printf "%d", $2/1024}' /proc/meminfo)
    per_job=1800; [ "${PSR_SMALL_CODE:-}" = OS ] && per_job=1200
    PSR_JOBS=$(( (mem_mb - 2500) / per_job )); cpus=$(nproc)
    [ "$PSR_JOBS" -gt "$cpus" ] && PSR_JOBS=$cpus
    [ "$PSR_JOBS" -lt 1 ] && PSR_JOBS=1
    echo "[build] ${mem_mb} MB free memory -> $PSR_JOBS parallel compile(s) (override with PSR_JOBS=N)"
fi
export PSR_JOBS="${PSR_JOBS:-$(nproc)}"

BUILD_DIR=build
if [ -n "${DEVKITPRO:-}" ] && [ -x "$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake" ] && [ "${PSR_USE_DOCKER:-0}" != 1 ]; then
    echo "[build] local devkitPro at $DEVKITPRO"
    cd switch
    mkdir -p "$BUILD_DIR"
    # PSR_CCACHE=1: remember compiled files (needs the ccache program). The FIRST build is not faster; a later build that has to
    # recompile everything (build folder deleted, PSR_SMALL_CODE changed and changed back, a failed run repeated from scratch)
    # takes minutes instead of hours. Cached objects live in ~/.cache/ccache; CCACHE_MAXSIZE below keeps them to 8 GB.
    CCACHE_ARGS=()
    if [ "${PSR_CCACHE:-0}" = 1 ]; then
        if command -v ccache >/dev/null 2>&1; then
            export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-8G}"
            CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
            echo "[build] ccache on (cache limit $CCACHE_MAXSIZE)"
        else
            echo "[build] PSR_CCACHE=1 but ccache is not installed (Ubuntu: sudo apt install ccache); building without it" >&2
        fi
    fi
    "$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake" -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release ${CCACHE_ARGS[@]+"${CCACHE_ARGS[@]}"}
    cmake --build "$BUILD_DIR" -j"$PSR_JOBS" 2>&1 | tee "$BUILD_DIR/build.log"
    cd ..
else
    command -v docker >/dev/null || { echo "need either devkitPro (DEVKITPRO set) or docker" >&2; exit 1; }
    echo "[build] docker devkitpro/devkita64"
    mkdir -p "switch/$BUILD_DIR"
    docker run --rm -e BUILD_DIR="$BUILD_DIR" -e PSR_JOBS="$PSR_JOBS" -e PSR_SMALL_CODE="${PSR_SMALL_CODE:-}" -e PSR_BIG_LANES="${PSR_BIG_LANES:-}" -e PSR_BIG_FILE_BYTES="${PSR_BIG_FILE_BYTES:-}" -v "$PWD:$PWD" -w "$PWD/switch" devkitpro/devkita64:latest bash -c '
        set -euo pipefail
        /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
        cmake --build "$BUILD_DIR" -j"$PSR_JOBS" 2>&1 | tee "$BUILD_DIR/build.log"
    '
fi
ls -la "switch/$BUILD_DIR/osaka-nx.nro"
# Sanity check: the NRO must contain the strings of the patches/features this tree expects (a stale NRO means the build failed above).
for S in "shader warm-up:" "build stamp:" "psrnx-shaderused-patch" "splash: shown"; do
    if grep -aq "$S" "switch/$BUILD_DIR/osaka-nx.nro"; then echo "[build] NRO contains '$S': yes"; else echo "[build] WARNING: NRO does NOT contain '$S' (stale or failed build?)" >&2; fi
done
