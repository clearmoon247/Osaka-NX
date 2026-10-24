#!/usr/bin/env bash
# Measures how long the Switch compiler needs for a few of YOUR generated/funcs_*.c files under different compiler settings,
# so that "make the build faster" can be decided from numbers instead of guesses. Nothing is changed: it only compiles into a
# temporary folder, and writes build/compile_time_experiment.txt (send that file back).
#
# Why this file exists
#     About 90% of the program is the recompiled game (generated/*.c, around a thousand functions in a few hundred huge files),
#     and compiling it is by far the longest part of a build. The settings in switch/CMakeLists.txt were chosen for the size of the
#     result and for run-time speed, not for compile time. This script shows, per file: seconds, peak memory, and the size of the
#     code (.text + .eh_frame), for several settings, so you can see what a faster setting costs. Peak memory also tells you how
#     many files can be compiled in parallel (PSR_JOBS) on your machine.
#
# Needs: a finished (or at least configured) build, so that the exact flags exist (switch/build/CMakeFiles/RecompiledFuncs.dir/flags.make),
#        generated/*.c, python3.
#
# Use
#     switch/tools/compile_time_experiment.sh                    files at the 50th, 75th and 90th size percentile
#     switch/tools/compile_time_experiment.sh funcs_9.c funcs_4.c   chosen files (funcs_4.c is known to be very slow at -O2)
#     PSR_EXP_TIMEOUT=900 ...                                    give up on one compile after this many seconds (default 600)
#     PSR_EXP_LIGHT=1 ...                                        only the first three settings (quicker)
#
# Settings tried (all without -g, with the control-flow probe compiled out, exactly like the real build):
#     Os            what build-nro.sh uses by default (PSR_SMALL_CODE=OS)
#     O2            what switch/all.sh and build-nro.sh --fast use (the line "flags of the real build" in the output shows which one
#                   your build folder was configured with)
#     O1            fewer optimisation passes
#     O0            no optimisation: the floor, shows the most compile time any setting could ever save
#     Os + ...      Os with groups of the most expensive optimisation passes switched off one group at a time
# Reading the result: a setting is only interesting if it is clearly faster than Os AND its code size and your own run-time test
# (does the game still run at full speed?) stay acceptable. Never switch the real build over on this table alone.
set -uo pipefail
cd "$(dirname "$0")/../.."          # repository root
ROOT=$PWD
TIMEOUT="${PSR_EXP_TIMEOUT:-600}"

# ---- Find the flags and the compiler of the real build (same lookup as size_experiment.sh) -------------------------------------
FLAGS=""; BUILD=""
for d in switch/build build; do
    [ -f "$d/CMakeFiles/RecompiledFuncs.dir/flags.make" ] && FLAGS="$d/CMakeFiles/RecompiledFuncs.dir/flags.make" && BUILD="$d" && break
done
[ -n "$FLAGS" ] || { echo "no flags.make found: run switch/build.sh once first (it can fail later; configuring is enough)" >&2; exit 1; }
CC=$(grep -m1 '^CMAKE_C_COMPILER:' "$BUILD/CMakeCache.txt" 2>/dev/null | sed 's/^[^=]*=//')
[ -x "${CC:-}" ] || CC=$(sed -n 's/^set(CMAKE_C_COMPILER "\(.*\)")$/\1/p' "$BUILD"/CMakeFiles/*/CMakeCCompiler.cmake 2>/dev/null | head -1)
[ -x "${CC:-}" ] || CC="${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin/aarch64-none-elf-gcc"
[ -x "$CC" ] || CC=$(command -v aarch64-none-elf-gcc || true)
[ -x "${CC:-}" ] || { echo "cannot find the C compiler (looked in CMakeCache.txt, CMakeCCompiler.cmake, \$DEVKITPRO)" >&2; exit 1; }
PREFIX="${CC%gcc}"
SIZE="${PREFIX}size"
command -v "$SIZE" >/dev/null 2>&1 || SIZE=size
command -v python3 >/dev/null 2>&1 || { echo "python3 is needed (it measures peak memory)" >&2; exit 1; }

DEFS=$(sed -n 's/^C_DEFINES = //p' "$FLAGS"); INCS=$(sed -n 's/^C_INCLUDES = //p' "$FLAGS"); CFL=$(sed -n 's/^C_FLAGS = //p' "$FLAGS")
BASE=$(echo "$CFL" | sed -E 's/(^| )-O[0-9sgz]( |$)/ /g; s/(^| )-g[0-9]?( |$)/ /g')       # without any -O level and without -g
OUT="$BUILD/compile_time_experiment.txt"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/shim"
# recomp.h with the control-flow probe compiled out: this is what the current build does.
printf '#include_next "recomp.h"\n#define recomp_cf_note(...) ((void)0)\n' > "$TMP/shim/recomp.h"
NOCF="-I$TMP/shim"

# ---- Which files ---------------------------------------------------------------------------------------------------------------
mapfile -t ALL < <(ls -S generated/funcs_*.c)
n=${#ALL[@]}
[ "$n" -gt 0 ] || { echo "generated/funcs_*.c missing" >&2; exit 1; }
if [ $# -ge 1 ]; then FILES=(); for f in "$@"; do FILES+=("generated/$(basename "$f")"); done
else FILES=("${ALL[$((n*50/100))]}" "${ALL[$((n*25/100))]}" "${ALL[$((n*10/100))]}"); fi

# ---- The settings --------------------------------------------------------------------------------------------------------------
# name|flags
VARIANTS=(
    "Os (build-nro.sh default)|-Os"
    "O2 (all.sh default, --fast)|-O2"
    "O1|-O1"
    "Os, no global CSE/PRE/hoisting|-Os -fno-gcse -fno-tree-pre -fno-code-hoisting"
    "Os, no VRP/jump threading|-Os -fno-tree-vrp -fno-tree-dominator-opts -fno-thread-jumps"
    "Os, no dead-store/sink passes|-Os -fno-tree-dse -fno-dse -fno-tree-sink"
    "Os, all three groups off|-Os -fno-gcse -fno-tree-pre -fno-code-hoisting -fno-tree-vrp -fno-tree-dominator-opts -fno-thread-jumps -fno-tree-dse -fno-dse -fno-tree-sink"
    "O0 (floor)|-O0"
)
[ "${PSR_EXP_LIGHT:-0}" = 1 ] && VARIANTS=("${VARIANTS[@]:0:3}")

# Runs one compile and prints: seconds  peak-memory-MB  exit-status. Compiler messages go to $TMP/err.txt.
# Python is used because it can report the peak memory of a child process (ru_maxrss), which plain shells cannot.
timed_compile() {
    python3 - "$TIMEOUT" "$@" <<'PY'
import os, resource, subprocess, sys, time
limit = int(sys.argv[1])
cmd = sys.argv[2:]
start = time.time()
try:
    with open(os.environ["PSR_EXP_ERR"], "wb") as err:
        status = subprocess.run(cmd, stderr=err, timeout=limit).returncode
except subprocess.TimeoutExpired:
    status = 124
print("%.1f %d %d" % (time.time() - start, resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss // 1024, status))
PY
}
export PSR_EXP_ERR="$TMP/err.txt"

{
echo "== compiler: $CC"
echo "== flags of the real build: $CFL"
echo "== per-compile time limit: ${TIMEOUT}s   (this machine: $(nproc) threads, $(awk '/MemAvailable/ {printf "%d", $2/1048576}' /proc/meminfo 2>/dev/null) GB free memory)"
echo "== generated/: $n files, $(du -sh generated | cut -f1) in total"
echo "== size spread (source bytes): largest $(wc -c < "${ALL[0]}"), 10th pct $(wc -c < "${ALL[$((n*10/100))]}"), median $(wc -c < "${ALL[$((n*50/100))]}"), smallest $(wc -c < "${ALL[$((n-1))]}")"
echo "== the five largest: $(for f in "${ALL[@]:0:5}"; do printf '%s (%s)  ' "$(basename "$f")" "$(wc -c < "$f")"; done)"
for SRC in "${FILES[@]}"; do
    [ -f "$SRC" ] || { echo "no such file: $SRC"; continue; }
    echo
    echo "== $SRC: $(wc -c < "$SRC") bytes of source, $(wc -l < "$SRC") lines"
    printf "  %-34s %7s %8s %11s %9s\n" setting secs "peak MB" ".text+eh" "vs Os"
    base_secs=""
    for entry in "${VARIANTS[@]}"; do
        name="${entry%%|*}"; fl="${entry#*|}"
        obj="$TMP/o.o"; rm -f "$obj"
        # shellcheck disable=SC2086
        read -r secs mb status < <(timed_compile "$CC" $fl $DEFS $INCS $BASE $NOCF -I"$ROOT/generated" -c "$SRC" -o "$obj")
        if [ "$status" = 0 ]; then
            sz=$("$SIZE" -A "$obj" 2>/dev/null | awk '$1 ~ /^\.(text|eh_frame)/ { t += $2 } END { print t + 0 }')
            [ -z "$base_secs" ] && base_secs="$secs"
            ratio=$(awk -v a="$secs" -v b="$base_secs" 'BEGIN { if (b > 0) printf "%.2fx", a / b; else print "-" }')
            printf "  %-34s %7s %8s %11s %9s\n" "$name" "$secs" "$mb" "$sz" "$ratio"
        elif [ "$status" = 124 ]; then
            printf "  %-34s   gave up after %ss (limit PSR_EXP_TIMEOUT)\n" "$name" "$TIMEOUT"
        else
            printf "  %-34s   FAILED: %s\n" "$name" "$(head -c 160 "$TMP/err.txt" | tr '\n' ' ')"
        fi
    done
    # Where the time goes at the default setting: the most expensive compiler passes (GCC -ftime-report, wall-clock column).
    echo "  -- the slowest compiler passes at Os for this file (GCC -ftime-report: seconds and share of total):"
    # shellcheck disable=SC2086
    "$CC" -Os -ftime-report $DEFS $INCS $BASE $NOCF -I"$ROOT/generated" -c "$SRC" -o "$TMP/t.o" 2> "$TMP/report.txt" || true
    awk -F: '/%\)/ && $1 !~ /TOTAL/ { split($2, a, " "); printf "%9.2f%s\n", a[1], $0 }' "$TMP/report.txt" | sort -rn | head -8 | cut -c10- | sed 's/^/     /'
done
echo
echo "== how to read this: the whole build is roughly the sum of this per file over $n files, divided by the number of parallel jobs."
echo "== peak MB x parallel jobs must fit in free memory; the build script uses free-GB / 3 jobs, which is safe but may leave processors idle if peak MB is well below 3000."
} 2>&1 | tee "$OUT"
echo "Wrote $OUT"
