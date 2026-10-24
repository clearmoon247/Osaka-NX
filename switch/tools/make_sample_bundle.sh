#!/usr/bin/env bash
# Bundles a handful of generated files plus the compile flags of your build into one small archive, so size experiments
# can be run elsewhere (on a machine with the same compiler) without a ROM, the 2.7 GB generated/ tree or a full build.
# Contains only generated C source (derived from your ROM): keep it private, like the ROM itself.
#   switch/tools/make_sample_bundle.sh            six files at the 90th, 75th, 50th, 25th, 10th and 2nd size percentile
#   switch/tools/make_sample_bundle.sh funcs_9.c  chosen files (appended to nothing else)
# Writes build/psr-samples.tar.gz (a few MB).
set -euo pipefail
cd "$(dirname "$0")/../.."
BUILD=""
for d in switch/build build; do [ -f "$d/CMakeFiles/RecompiledFuncs.dir/flags.make" ] && BUILD="$d" && break; done
[ -n "$BUILD" ] || { echo "no flags.make found: build first (switch/build.sh)" >&2; exit 1; }
[ -d generated ] || { echo "generated/ missing: run switch/codegen.sh" >&2; exit 1; }

if [ $# -ge 1 ]; then FILES=(); for f in "$@"; do FILES+=("generated/$(basename "$f")"); done
else
    mapfile -t ALL < <(ls -S generated/funcs_*.c)
    n=${#ALL[@]}
    FILES=(); for p in 10 25 50 75 90 98; do FILES+=("${ALL[$((n*p/100))]}"); done
fi
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/psr-samples/generated" "$STAGE/psr-samples/build"
for f in "${FILES[@]}"; do [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }; cp "$f" "$STAGE/psr-samples/generated/"; done
# every non-code header the generated C files include from generated/ (funcs.h and friends)
cp generated/*.h "$STAGE/psr-samples/generated/" 2>/dev/null || true
cp "$BUILD/CMakeFiles/RecompiledFuncs.dir/flags.make" "$STAGE/psr-samples/build/flags.make"
[ -f "$BUILD/CMakeCache.txt" ] && grep -E '^(CMAKE_C_COMPILER|CMAKE_BUILD_TYPE|PSR_[A-Z_]*):' "$BUILD/CMakeCache.txt" > "$STAGE/psr-samples/build/cache-excerpt.txt" || true
{ echo "files:"; for f in "${FILES[@]}"; do echo "  $(basename "$f") $(stat -c %s "$f") bytes, $(grep -c '^RECOMP_FUNC' "$f" 2>/dev/null || true) RECOMP_FUNCs"; done
  echo "total generated: $(ls generated/funcs_*.c | wc -l) files"; ${CC:-true} --version 2>/dev/null | head -1; } > "$STAGE/psr-samples/README.txt"
OUT="$BUILD/psr-samples.tar.gz"
tar czf "$OUT" -C "$STAGE" psr-samples
ls -la "$OUT"; cat "$STAGE/psr-samples/README.txt"
echo "Send $OUT back (it is derived from your ROM: do not share it publicly)."
