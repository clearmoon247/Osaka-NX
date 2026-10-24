#!/usr/bin/env bash
# One command on an x86_64 Linux host (WSL2 Ubuntu 24.04 is fine): deps -> codegen -> .nro.
#   switch/all.sh /path/to/Pokemon_Stadium__USA_.z64
# Every step logs to logs/; send me logs/ (or the tail of the failing one) if something breaks.
set -euo pipefail
ROM="${1:?usage: switch/all.sh <Pokemon Stadium (US v1.0) .z64>}"
cd "$(dirname "$0")/.."
mkdir -p logs
# stage <number> <name> <command...>: runs one stage, logs it, and remembers how long it took.
TIMES=()
stage() {
    local n="$1" name="$2" t0=$SECONDS; shift 2
    echo "== $name"
    "$@" 2>&1 | tee "logs/$n-$name.log"
    [ "${PIPESTATUS[0]}" -eq 0 ] || { echo "stage $name failed: see logs/$n-$name.log" >&2; exit 1; }
    TIMES+=("$(printf '  %-11s %4d min %2d s' "$name" $(( (SECONDS - t0) / 60 )) $(( (SECONDS - t0) % 60 )))")
}
stage 1 setup_deps switch/setup_deps.sh
stage 2 codegen    switch/codegen.sh "$ROM"
stage 3 build      switch/build.sh
echo "Done: switch/build/osaka-nx.nro"
echo "Where the time went:"; printf '%s\n' "${TIMES[@]}"
