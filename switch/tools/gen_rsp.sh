#!/usr/bin/env bash
# Makes rsp/aspMain.cpp, rsp/gbTowerMain.cpp, rsp/gbTowerColorMain.cpp and rsp/njpgdspMain.cpp from YOUR ROM.
#
# Why: these four files are the game's small RSP programs (audio, Game Boy Tower, JPEG) translated to C++. That is program code
# derived from the game, so Osaka-NX does not store it (see switch/NOTICE.md); every build makes it on the user's own machine,
# the same way generated/ is made. The .gitignore lists them. The program that translates them is RSPRecomp, from the same
# N64Recomp checkout that switch/codegen.sh builds.
#
# Steps (each one is plain and can be read in the toml files it names):
#   1. aspmain_combined.bin = the two small pieces of aspMain's code copied out of the ROM (tools/build_aspmain_combined.py)
#   2. RSPRecomp aspMain.us.1.0.toml               -> rsp/aspMain.cpp
#      RSPRecomp rsp/gbTowerMain.us.rev1.toml      -> rsp/gbTowerMain.cpp
#      RSPRecomp rsp/gbTowerColorMain.us.rev1.toml -> rsp/gbTowerColorMain.cpp
#      RSPRecomp rsp/njpgdspMain.us.rev1.toml      -> rsp/njpgdspMain.cpp
#   3. switch/tools/patch_aspmain.py: three small hand edits to the end of aspMain.cpp that the upstream project made
#
# Usage (from switch/codegen.sh; the ROM must already be copied to baserom.z64 in the repository root):
#   switch/tools/gen_rsp.sh <path to the RSPRecomp program> [python]
# Skipped when all four files exist. PSR_REGEN_RSP=1 makes them again.
set -euo pipefail

RSPRECOMP="${1:?usage: switch/tools/gen_rsp.sh <path to RSPRecomp> [python]}"
PY="${2:-python3}"
cd "$(dirname "$0")/../.."
OUTPUTS=(rsp/aspMain.cpp rsp/gbTowerMain.cpp rsp/gbTowerColorMain.cpp rsp/njpgdspMain.cpp)

if [ "${PSR_REGEN_RSP:-0}" != 1 ]; then
    all=1; for f in "${OUTPUTS[@]}"; do [ -s "$f" ] || all=0; done
    if [ "$all" = 1 ]; then echo "[gen_rsp] rsp/*.cpp already present (PSR_REGEN_RSP=1 makes them again)"; exit 0; fi
fi
[ -f baserom.z64 ] || { echo "[gen_rsp] baserom.z64 is missing in $PWD (switch/codegen.sh copies your ROM there)" >&2; exit 1; }
[ -x "$RSPRECOMP" ] || { echo "[gen_rsp] RSPRecomp not found or not executable: $RSPRECOMP" >&2; exit 1; }

echo "[gen_rsp] translating the game's RSP programs from your ROM"
rm -f "${OUTPUTS[@]}"
# A half-made set must never look finished: remove whatever was written if any step below fails.
trap 'rc=$?; if [ $rc -ne 0 ]; then rm -f "${OUTPUTS[@]}"; echo "[gen_rsp] failed; no rsp/*.cpp left behind" >&2; fi' EXIT
"$PY" -I tools/build_aspmain_combined.py --rom baserom.z64 --out aspmain_combined.bin
"$RSPRECOMP" aspMain.us.1.0.toml
"$RSPRECOMP" rsp/gbTowerMain.us.rev1.toml
"$RSPRECOMP" rsp/gbTowerColorMain.us.rev1.toml
"$RSPRECOMP" rsp/njpgdspMain.us.rev1.toml
"$PY" -I switch/tools/patch_aspmain.py rsp/aspMain.cpp
for f in "${OUTPUTS[@]}"; do
    [ -s "$f" ] || { echo "[gen_rsp] $f was not produced" >&2; exit 1; }
done
echo "[gen_rsp] made ${OUTPUTS[*]}"
