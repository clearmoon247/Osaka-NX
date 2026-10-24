#!/usr/bin/env bash
# Generates generated/*.c (the recompiled game) from YOUR OWN Pokemon Stadium (US v1.0) ROM. Nothing derived from the
# ROM is tracked in git (generated/ and baserom.z64 are gitignored upstream).
#
# Needs an x86_64 Linux host (native, WSL2, or the container below): pret's build runs IDO, which is x86 only.
#   native / WSL2 (Ubuntu 24.04):   switch/codegen.sh /path/to/Pokemon_Stadium__USA_.z64
#   container:  docker run --rm -v "$PWD:/work" -v "<rom dir>:/rom:ro" ubuntu:24.04 \
#                   bash /work/switch/codegen.sh /rom/Pokemon_Stadium__USA_.z64
#
# Stages (each is skipped if its output exists; delete the output to redo it):
#   1. pret/pokestadium at the commit upstream pins (disasm submodule), `make init && make` -> disasm/build/pokestadium-us.elf
#   2. N64Recomp CLI at the sha in n64recomp.pin
#   3. N64Recomp game.toml -> generated/ (skipped if generated/lookup.cpp exists; PSR_REGEN=1 forces it)
#   4. RSPRecomp -> rsp/*.cpp, the game's audio, Game Boy Tower and JPEG microcode (switch/tools/gen_rsp.sh; skipped if the files
#      exist; PSR_REGEN_RSP=1 forces it)
#
# Needs an x86_64 Linux host with MIPS binutils (binutils-mips-linux-gnu), Python 3 with venv, git and network access to GitHub
# and PyPI. Nothing derived from the ROM is ever committed: generated/ is git-ignored and stays on your machine.
set -euo pipefail

ROM="${1:?usage: switch/codegen.sh <path to Pokemon Stadium (US v1.0) .z64>}"
cd "$(dirname "$0")/.."
ROOT="$PWD"
EXPECTED_MD5=ed1378bc12115f71209a77844965ba50
N64RECOMP_COMMIT=$(sed -n 's/^[[:space:]]*sha[[:space:]]*=[[:space:]]*//p' n64recomp.pin | tr -d '[:space:]')
N64RECOMP_REPO=https://github.com/mstan/N64Recomp.git

if [ "$(uname -m)" != x86_64 ]; then
    echo "x86_64 host required (pret's IDO compiler); this is $(uname -m)" >&2
    exit 1
fi
[ "$(md5sum "$ROM" | cut -d' ' -f1)" = "$EXPECTED_MD5" ] || {
    echo "ROM is not Pokemon Stadium (US) v1.0: md5 must be $EXPECTED_MD5 (Rev A / v1.1 will not work)" >&2
    exit 1
}

# ---- packages (only as root, i.e. in the container; elsewhere install them yourself) ------------------------------
if [ "$(id -u)" = 0 ] && command -v apt-get >/dev/null; then
    apt-get -qq update
    DEBIAN_FRONTEND=noninteractive apt-get -qq install -y build-essential git cmake ninja-build binutils-mips-linux-gnu \
        python3 python3-pip python3-venv >/dev/null
fi
for tool in git make cmake python3 mips-linux-gnu-ld; do
    command -v "$tool" >/dev/null || { echo "missing: $tool (Ubuntu: sudo apt install build-essential git cmake ninja-build binutils-mips-linux-gnu python3 python3-pip python3-venv)" >&2; exit 1; }
done

# ---- 1. pret disassembly build ------------------------------------------------------------------------------------
cp "$ROM" baserom.z64                       # game.toml: rom_file_path = "baserom.z64"
if [ ! -f disasm/build/pokestadium-us.elf ]; then
    git submodule update --init disasm      # the commit upstream pins (756f7e3 when this was written)
    mkdir -p disasm/baseroms/us
    cp "$ROM" disasm/baseroms/us/baserom.z64
    # tools/disasm_fragment78_split.patch moves one fragment's vram; game.toml still lists frag_8FF00000 at its old
    # address, so it is NOT applied by default. PSR_APPLY_FRAG78_PATCH=1 applies it (unverified either way).
    if [ "${PSR_APPLY_FRAG78_PATCH:-0}" = 1 ]; then
        git -C disasm apply --check "$ROOT/tools/disasm_fragment78_split.patch" && \
            git -C disasm apply "$ROOT/tools/disasm_fragment78_split.patch"
    fi
    python3 -m venv .venv-codegen
    # shellcheck disable=SC1091
    . .venv-codegen/bin/activate
    python3 -m pip install -q -r disasm/requirements.txt
    make -C disasm init
    make -C disasm -j"$(nproc)"
    deactivate
fi
[ -f disasm/build/pokestadium-us.elf ] || { echo "pret build did not produce disasm/build/pokestadium-us.elf" >&2; exit 1; }

# ---- 2. N64Recomp CLI -----------------------------------------------------------------------------------------------
TOOLS="$ROOT/switch/tools"
if [ ! -x "$TOOLS/N64Recomp" ] || [ ! -x "$TOOLS/RSPRecomp" ]; then
    mkdir -p "$TOOLS"
    if [ -d "$ROOT/lib/N64ModernRuntime/N64Recomp/.git" ] || [ -f "$ROOT/lib/N64ModernRuntime/N64Recomp/.git" ]; then
        SRC="$ROOT/lib/N64ModernRuntime/N64Recomp"          # already fetched by switch/setup_deps.sh
    else
        SRC="$TOOLS/src"
        [ -d "$SRC/.git" ] || git clone -q "$N64RECOMP_REPO" "$SRC"
    fi
    git -C "$SRC" checkout -q "$N64RECOMP_COMMIT"
    for sub in lib/rabbitizer lib/ELFIO lib/fmt lib/tomlplusplus lib/sljit; do
        git -C "$SRC" submodule update -q --init --recursive "$sub"
    done
    cmake -S "$SRC" -B "$TOOLS/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$TOOLS/build" --target N64RecompCLI RSPRecomp
    cp "$TOOLS/build/N64Recomp" "$TOOLS/N64Recomp"
    cp "$TOOLS/build/RSPRecomp" "$TOOLS/RSPRecomp"
fi

# ---- 3. recompile -----------------------------------------------------------------------------------------------------
# Regenerating rewrites every generated/*.c, so make would recompile all ~1000 files (hours). Skip it when generated/
# is already complete, unless PSR_REGEN=1.
if [ ! -f generated/lookup.cpp ] || [ "${PSR_REGEN:-0}" = 1 ]; then
    rm -rf generated
    mkdir -p generated
    "$TOOLS/N64Recomp" game.toml
fi
# ---- 4. smaller call sites ------------------------------------------------------------------------------------------
# The recompiler repeats ~20 lines after each of ~1.1 million guest calls; switch/tools/compact_calls.py folds them into
# calls of switch/call_post_switch.c (same behaviour, about 30% less code). Idempotent, ~30 s. PSR_COMPACT_CALLS=0 skips it.
if [ "${PSR_COMPACT_CALLS:-1}" = 1 ]; then
    python3 -I "$ROOT/switch/tools/compact_calls.py" generated/funcs_*.c
fi
# ---- 5. the game's RSP microcode ---------------------------------------------------------------------------------------
# rsp/aspMain.cpp, gbTowerMain.cpp, gbTowerColorMain.cpp, njpgdspMain.cpp: translated from your ROM here, not stored in the
# repository (see switch/NOTICE.md). Quick (a second or two).
"$ROOT/switch/tools/gen_rsp.sh" "$TOOLS/RSPRecomp" python3
for f in generated/lookup.cpp generated/recomp_overlays.inl; do
    [ -f "$f" ] || { echo "N64Recomp did not produce $f" >&2; exit 1; }
done
echo "Generated $(ls generated/*.c | wc -l) C files in generated/ (expect ~hundreds; upstream reports 1007 functions across 77 fragments)."
