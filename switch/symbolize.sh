#!/usr/bin/env bash
# Turns the stall sampler's "prof" lines of osaka-nx.log into function names (see symbolize.py).
#   switch/symbolize.sh /path/to/osaka-nx.log [stalled_ms]
# The result is also written to symbolized.txt next to the log: attach that file together with the logs.
set -euo pipefail
LOG="${1:?usage: symbolize.sh <osaka-nx.log> [stalled_ms]}"
OUT="$(dirname "$LOG")/symbolized.txt"
python3 "$(dirname "$0")/symbolize.py" "$@" | tee "$OUT"
echo "(saved to $OUT)"
