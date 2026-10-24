#!/usr/bin/env bash
# Fetches the third-party sources the Switch build compiles into lib/ and applies switch/patches/. Needs git and
# network access to GitHub only. Safe to re-run (patches are applied once).
#
#   lib/N64ModernRuntime   mstan/N64ModernRuntime at N64MR_COMMIT, with N64Recomp at the sha in n64recomp.pin
#   lib/GLideN64           gonetz/GLideN64 at GLIDEN64_COMMIT (the revision DK64Recomp-NX runs on the console)
#
# The runtime revision upstream PokemonStadiumRecomp builds against is NOT recorded anywhere in its repository
# (lib/N64ModernRuntime is a junction to the author's local checkout). d4bf882 (2026-07-04, main tip when this port
# was started) contains every runtime symbol the upstream runner uses except ultramodern::input::apply_stick_deadzone,
# which exists in no published revision (switch/input_switch.cpp has its own).
set -euo pipefail
cd "$(dirname "$0")/.."

N64MR_REPO=https://github.com/mstan/N64ModernRuntime.git
N64MR_COMMIT=d4bf882
N64RECOMP_REPO=https://github.com/mstan/N64Recomp.git
N64RECOMP_COMMIT=$(sed -n 's/^[[:space:]]*sha[[:space:]]*=[[:space:]]*//p' n64recomp.pin | tr -d '[:space:]')
GLIDEN64_REPO=https://github.com/gonetz/GLideN64.git
GLIDEN64_COMMIT=41c7ba273a6c9afb43c0574cf3cf5d139182d070

[ -n "$N64RECOMP_COMMIT" ] || { echo "no sha in n64recomp.pin" >&2; exit 1; }
mkdir -p lib

# ---- N64ModernRuntime -------------------------------------------------------------------------------------------
if [ ! -d lib/N64ModernRuntime/.git ]; then
    git clone -q "$N64MR_REPO" lib/N64ModernRuntime
fi
git -C lib/N64ModernRuntime checkout -q "$N64MR_COMMIT"
# Its own submodules, except N64Recomp (pinned separately below).
for sub in thirdparty/xxHash thirdparty/miniz thirdparty/o1heap; do
    git -C lib/N64ModernRuntime submodule update -q --init "$sub"
done
if [ ! -d lib/N64ModernRuntime/N64Recomp/.git ] && [ ! -f lib/N64ModernRuntime/N64Recomp/.git ]; then
    rm -rf lib/N64ModernRuntime/N64Recomp
    git clone -q "$N64RECOMP_REPO" lib/N64ModernRuntime/N64Recomp
fi
git -C lib/N64ModernRuntime/N64Recomp checkout -q "$N64RECOMP_COMMIT"
# N64Recomp's submodules, except the optional Ares oracle (large, unused here).
for sub in lib/rabbitizer lib/ELFIO lib/fmt lib/tomlplusplus lib/sljit; do
    git -C lib/N64ModernRuntime/N64Recomp submodule update -q --init --recursive "$sub"
done

# ---- GLideN64 ---------------------------------------------------------------------------------------------------
if [ ! -d lib/GLideN64/.git ]; then
    git init -q lib/GLideN64
    git -C lib/GLideN64 remote add origin "$GLIDEN64_REPO"
fi
if [ "$(git -C lib/GLideN64 rev-parse -q --verify HEAD 2>/dev/null || true)" != "$GLIDEN64_COMMIT" ]; then
    git -C lib/GLideN64 fetch -q --depth 1 origin "$GLIDEN64_COMMIT"
    git -C lib/GLideN64 checkout -q FETCH_HEAD
fi

# ---- patches (kept out of the checkouts' history so the pins stay exact) ------------------------------------------
# One patch per dependency: switch/patches/n64modernruntime.patch and switch/patches/gliden64.patch (each includes the small
# fixes the devkitA64 newlib toolchain needs, e.g. <cstddef> for size_t in audio_uaf_protect.hpp).
apply_patch() {  # <checkout dir> <patch file>
    if git -C "$1" apply --reverse --check "$PWD/$2" 2>/dev/null; then
        echo "already applied: $2"
    elif git -C "$1" apply --check "$PWD/$2" 2>/dev/null; then
        git -C "$1" apply "$PWD/$2"
        echo "applied: $2"
    else
        echo "ERROR: $2 does not apply to $1 (it has local changes, or an older Osaka-NX patch set is applied)." >&2
        echo "       Reset the checkout and run this script again:  git -C $1 checkout -- . && switch/setup_deps.sh" >&2
        exit 1
    fi
}
apply_patch lib/N64ModernRuntime switch/patches/n64modernruntime.patch
apply_patch lib/GLideN64 switch/patches/gliden64.patch

echo "lib/ ready: N64ModernRuntime $(git -C lib/N64ModernRuntime rev-parse --short HEAD)," \
     "N64Recomp $(git -C lib/N64ModernRuntime/N64Recomp rev-parse --short HEAD)," \
     "GLideN64 $(git -C lib/GLideN64 rev-parse --short HEAD)"
