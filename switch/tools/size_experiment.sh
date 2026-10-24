#!/usr/bin/env bash
# Compiles a few generated files with the exact flags of the real build under several settings and reports the object size
# of each, so the effect of a flag or a code change can be judged in minutes instead of a full rebuild. Needs a finished
# build (build/CMakeFiles/RecompiledFuncs.dir/flags.make) and generated/*.c. Writes build/size_experiment.txt - send it back.
#   switch/tools/size_experiment.sh                    four files: sizes at the 25th, 50th, 75th and 95th percentile
#   switch/tools/size_experiment.sh funcs_9.c funcs_300.c   chosen files
set -uo pipefail
cd "$(dirname "$0")/../.."          # repository root
ROOT=$PWD
FLAGS=build_dir_unset
for d in switch/build build; do [ -f "$d/CMakeFiles/RecompiledFuncs.dir/flags.make" ] && FLAGS="$d/CMakeFiles/RecompiledFuncs.dir/flags.make" && BUILD="$d" && break; done
[ -f "$FLAGS" ] || { echo "no flags.make found: build first (switch/build.sh)" >&2; exit 1; }
CC=$(grep -m1 '^CMAKE_C_COMPILER:' "$BUILD/CMakeCache.txt" | sed 's/^[^=]*=//')
[ -x "$CC" ] || CC=$(sed -n 's/^set(CMAKE_C_COMPILER "\(.*\)")$/\1/p' "$BUILD"/CMakeFiles/*/CMakeCCompiler.cmake 2>/dev/null | head -1)
[ -x "$CC" ] || CC="${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin/aarch64-none-elf-gcc"
[ -x "$CC" ] || CC=$(command -v aarch64-none-elf-gcc || true)
[ -x "$CC" ] || { echo "cannot find the C compiler (looked in CMakeCache.txt, CMakeCCompiler.cmake, \$DEVKITPRO)" >&2; exit 1; }
PREFIX="${CC%gcc}"
SIZE="${PREFIX}size"; NM="${PREFIX}nm"; OBJDUMP="${PREFIX}objdump"

if [ $# -ge 1 ]; then FILES=(); for f in "$@"; do FILES+=("generated/$(basename "$f")"); done
else
    mapfile -t ALL < <(ls -S generated/funcs_*.c)
    n=${#ALL[@]}; FILES=("${ALL[$((n*75/100))]}" "${ALL[$((n*50/100))]}" "${ALL[$((n*25/100))]}" "${ALL[$((n*5/100))]}")
fi
DEFS=$(sed -n 's/^C_DEFINES = //p' "$FLAGS"); INCS=$(sed -n 's/^C_INCLUDES = //p' "$FLAGS"); CFL=$(sed -n 's/^C_FLAGS = //p' "$FLAGS")
BASE=$(echo "$CFL" | sed -E 's/(^| )-O[0-9sgz]( |$)/ /g; s/(^| )-g[0-9]?( |$)/ /g')      # without any -O level and without -g
OUT="$BUILD/size_experiment.txt"
TMP=$(mktemp -d)
mkdir -p "$TMP/shim"
# recomp.h with the control-flow diagnostics probe (recomp_cf_note) compiled out: this is what the current build does.
printf '#include_next "recomp.h"\n#define recomp_cf_note(...) ((void)0)\n' > "$TMP/shim/recomp.h"
NOCF="-I$TMP/shim"

# measure SRC NAME FLAGS...  -> prints one table row; sets LAST_TOTAL
measure() {
    local src="$1" name="$2"; shift 2
    local obj="$TMP/o_$(echo "$name" | tr -c 'A-Za-z0-9\n' _).o" t0 t1 text eh fn
    t0=$(date +%s.%N)
    # shellcheck disable=SC2086
    if "$CC" "$@" $DEFS $INCS $BASE -I"$ROOT/generated" -c "$src" -o "$obj" 2> "$TMP/err.txt"; then
        t1=$(date +%s.%N)
        text=$("$SIZE" -A "$obj" | awk '$1 ~ /^\.text/ { t += $2 } END { print t + 0 }')
        eh=$("$SIZE" -A "$obj" | awk '$1 ~ /^\.eh_frame/ { t += $2 } END { print t + 0 }')
        fn=$("$NM" --defined-only "$obj" | awk '$2 == "T" || $2 == "t"' | wc -l)
        LAST_TOTAL=$((text + eh)); LAST_OBJ="$obj"
        printf "  %-34s %9d %9d %9d %8d %7.1f\n" "$name" "$text" "$eh" "$LAST_TOTAL" "$fn" "$(echo "$t1 - $t0" | bc)"
    else
        printf "  %-34s FAILED: %s\n" "$name" "$(head -c 200 "$TMP/err.txt" | tr '\n' ' ')"
    fi
}
# Same function signatures with C99 restrict on rdram and ctx: lets the compiler keep ctx->rN in registers across stores to
# rdram (without it every guest store may alias the context, forcing reloads). EXPERIMENT: a size/speed probe, not yet a build option.
restrict_copy() { sed 's/(uint8_t\* rdram, recomp_context\* ctx) {/(uint8_t* __restrict rdram, recomp_context* __restrict ctx) {/' "$1" > "$TMP/r_$(basename "$1")"; echo "$TMP/r_$(basename "$1")"; }

{
echo "== compiler: $CC"
echo "== flags of the real build: $CFL"
echo "== generated/: $(ls generated/*.c | wc -l) C files, $(du -sh generated | cut -f1) in total"
echo "== all rows are .text + .eh_frame in bytes; -g is left out of every row"
for SRC in "${FILES[@]}"; do
    echo
    echo "== $SRC: $(wc -c < "$SRC") bytes of source, $(wc -l < "$SRC") lines, $(grep -c ' jal ' "$SRC") jal, $(grep -c 'jr ' "$SRC") jr"
    printf "  %-34s %9s %9s %9s %8s %7s\n" variant ".text" ".eh_frame" "total" "funcs" "secs"
    measure "$SRC" "O2 with probe (original build)" -O2
    BASE_TOTAL=$LAST_TOTAL
    measure "$SRC" "O2 no probe" -O2 $NOCF
    measure "$SRC" "Os no probe (current build)" -Os $NOCF
    CUR_OBJ="$LAST_OBJ"; CUR_TOTAL=$LAST_TOTAL
    RS=$(restrict_copy "$SRC")
    measure "$RS" "O2 no probe + restrict" -O2 $NOCF
    measure "$RS" "Os no probe + restrict" -Os $NOCF
    echo "  (current build is $((100 * CUR_TOTAL / BASE_TOTAL))% of the original size for this file)"
    if [ "$SRC" = "${FILES[1]:-}" ] || [ ${#FILES[@]} -eq 1 ]; then
        echo
        echo "== disassembly of a mid-sized function in the current build's object (first 70 lines): where do the bytes go?"
        f=$("$NM" --defined-only -S --size-sort --radix=d "$CUR_OBJ" 2>/dev/null | awk '$2 > 500 && $3 ~ /^[Tt]$/ { print $4; exit }')
        [ -n "$f" ] && "$OBJDUMP" -d --disassemble="$f" "$CUR_OBJ" | head -70
    fi
done
} 2>&1 | tee "$OUT"
rm -rf "$TMP"
echo "Wrote $OUT"
