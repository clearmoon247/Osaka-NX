#!/usr/bin/env bash
# EXPERIMENTAL - never run on Windows by its author. Windows-native version of switch/codegen.sh: turns YOUR OWN Pokemon Stadium
# (US v1.0) ROM into generated/*.c without WSL, a virtual machine or any Linux system.
#
# Where it runs: inside MSYS2 (a small Unix-style shell for Windows). switch/build-windows-native.ps1 installs MSYS2 and the
# other tools for you and then calls this script; you normally do not run it by hand.
#
# Why a separate script: switch/codegen.sh assumes Linux (it installs packages with apt, uses Linux build programs and the
# Linux layout of a Python virtual environment). The steps are the same, only the tools differ:
#
#   Linux (codegen.sh)                       Windows-native (this file)
#   ---------------------------------------  ----------------------------------------------------------------------------
#   pret's IDO compiler, Linux binaries      the same compiler recompiled for Windows (decompals/ido-static-recomp release)
#   binutils-mips-linux-gnu (apt)            mips64-elf binutils for Windows (n64-tools/gcc-toolchain-mips64)
#   python3 from apt, venv/bin               python.org Python for Windows, venv/Scripts
#   N64Recomp built with Ninja + gcc         N64Recomp built with Ninja + MinGW-w64 gcc (from MSYS2)
#
# What it does, in order (each stage is skipped when its result already exists, so you can run it again after a failure):
#   1. check the ROM's checksum and the tools
#   2. fetch pret/pokestadium (the "disasm" submodule) and put your ROM and the Windows IDO compiler into it
#   3. build pret's project, which compiles the game's C source with IDO and links pokestadium-us.elf.
#      pret's build then compares the ROM it rebuilt with yours byte for byte. If that comparison passes, the Windows tools
#      are known to produce exactly the same code as the Linux tools - so a mismatch stops the build here, by design.
#   4. build the N64Recomp tool
#   5. run N64Recomp on the ELF -> generated/ (stays on your machine; never commit or share it)
#   6. shrink the call sites (switch/tools/compact_calls.py), as codegen.sh does
#   7. run RSPRecomp -> rsp/*.cpp, the game's audio, Game Boy Tower and JPEG microcode (switch/tools/gen_rsp.sh), as codegen.sh does
#
# Needs these variables (switch/build-windows-native.ps1 sets them; paths are MSYS style, e.g. /c/OsakaNX/python/python.exe):
#   OSAKA_PY          Windows Python 3.12 (python.exe), used for pret's tools and for compact_calls.py
#   OSAKA_MIPS_BIN    folder with mips64-elf-as, mips64-elf-ld, mips64-elf-objcopy ...
#   OSAKA_IDO71       ido-7.1-recomp-windows.tar.gz        OSAKA_IDO53   ido-5.3-recomp-windows.tar.gz
# Optional: PSR_JOBS (parallel compiles), PSR_REGEN=1 (redo stage 5), PSR_COMPACT_CALLS=0 (skip stage 6),
#           PSR_APPLY_FRAG78_PATCH=1 (same meaning as in codegen.sh).
set -euo pipefail

ROM="${1:?usage: switch/codegen-msys.sh <path to Pokemon Stadium (US v1.0) in .z64 byte order>}"
cd "$(dirname "$0")/.."
ROOT="$PWD"
EXPECTED_MD5=ed1378bc12115f71209a77844965ba50
N64RECOMP_COMMIT=$(sed -n 's/^[[:space:]]*sha[[:space:]]*=[[:space:]]*//p' n64recomp.pin | tr -d '[:space:]')
N64RECOMP_REPO=https://github.com/mstan/N64Recomp.git
JOBS="${PSR_JOBS:-$(nproc)}"

say() { echo "[codegen-msys] $*"; }
die() { echo "[codegen-msys] ERROR: $*" >&2; exit 1; }

# ---- 1. checks --------------------------------------------------------------------------------------------------------------
case "$(uname -s)" in MSYS*) ;; *) die "this script is for MSYS2 on Windows (uname says $(uname -s)); on Linux use switch/codegen.sh" ;; esac
[ "$(md5sum "$ROM" | cut -d' ' -f1)" = "$EXPECTED_MD5" ] || die "ROM is not Pokemon Stadium (US) v1.0 in .z64 byte order: md5 must be $EXPECTED_MD5"
for v in OSAKA_PY OSAKA_MIPS_BIN OSAKA_IDO71 OSAKA_IDO53; do
    [ -n "${!v:-}" ] || die "$v is not set (start this through switch/build-windows-native.ps1)"
done
[ -f "$OSAKA_PY" ]   || die "Python not found at $OSAKA_PY"
[ -f "$OSAKA_IDO71" ] && [ -f "$OSAKA_IDO53" ] || die "IDO archives not found ($OSAKA_IDO71, $OSAKA_IDO53)"
export PATH="/usr/bin:$OSAKA_MIPS_BIN:$PATH"       # /usr/bin first: the mips folder also contains a make.exe that must NOT win
for tool in git make gcc cpp iconv mips64-elf-as mips64-elf-ld mips64-elf-objcopy mips64-elf-ar mips64-elf-strip; do
    command -v "$tool" >/dev/null || die "missing tool: $tool"
done

# ---- 2. sources ------------------------------------------------------------------------------------------------------------
cp "$ROM" baserom.z64                                  # game.toml: rom_file_path = "baserom.z64"
if [ ! -f disasm/build/pokestadium-us.elf ]; then
    say "fetching pret/pokestadium (git submodule disasm)"
    git submodule update --init disasm
    mkdir -p disasm/baseroms/us
    cp "$ROM" disasm/baseroms/us/baserom.z64
    if [ "${PSR_APPLY_FRAG78_PATCH:-0}" = 1 ]; then
        git -C disasm apply --check "$ROOT/tools/disasm_fragment78_split.patch" && git -C disasm apply "$ROOT/tools/disasm_fragment78_split.patch"
    fi

    # The IDO compiler. pret's own copies are Linux and macOS programs; we add a "windows" folder with the Windows build and
    # tell pret's Makefile (DETECTED_OS=windows below) to use it. 7.1 compiles most files, 5.3 a few old ones and libultra.
    for v in 7.1 5.3; do
        dest="disasm/tools/ido/windows/$v"
        if [ ! -f "$dest/cc.exe" ]; then
            say "installing IDO $v (Windows build) into $dest"
            mkdir -p "$dest"
            if [ "$v" = 7.1 ]; then tar xzf "$OSAKA_IDO71" -C "$dest"; else tar xzf "$OSAKA_IDO53" -C "$dest"; fi
        fi
        [ -f "$dest/cc.exe" ] || die "IDO $v did not unpack (no cc.exe in $dest)"
    done

    # Python environment for pret's tools (splat, asm-processor, ...). Created here so we control which Python is used.
    if [ ! -f disasm/.venv/Scripts/python.exe ]; then
        say "creating the Python environment for pret's tools"
        "$OSAKA_PY" -m venv disasm/.venv
    fi
    # pret's Makefile and some scripts say "python3"; Windows Python only installs "python.exe", so make a copy under that name.
    [ -f disasm/.venv/Scripts/python3.exe ] || cp disasm/.venv/Scripts/python.exe disasm/.venv/Scripts/python3.exe
    export PATH="$ROOT/disasm/.venv/Scripts:$PATH"

    # ---- 3. pret's build ------------------------------------------------------------------------------------------------
    # pret's Makefile refuses to run when it sees Windows (OS=Windows_NT) and does not know the layout of a Windows Python
    # environment. Instead of editing it we override these on the command line:
    #   OS=MSYS                       skip the "Windows unsupported" error and the Linux/macOS detection
    #   DETECTED_OS=windows           use tools/ido/windows/... (installed above)
    #   VENV_BIN_DIR=Scripts, PYTHON  Windows layout of the Python environment
    #   MIPS_BINUTILS_PREFIX          which binutils to use
    #   RUN_CC_CHECK=0                skip the optional extra syntax check with a host "gcc -m32" (MSYS gcc cannot do -m32)
    MAKE_ARGS=(OS=MSYS DETECTED_OS=windows VENV_BIN_DIR=Scripts PYTHON=.venv/Scripts/python.exe
               MIPS_BINUTILS_PREFIX=mips64-elf- RUN_CC_CHECK=0 N_THREADS="$JOBS")
    say "installing pret's Python packages, building its helper tools and extracting assets from your ROM (several minutes)"
    make -C disasm "${MAKE_ARGS[@]}" venv setup extract
    say "compiling the game with IDO and linking the ELF (this is the long step: expect 20-60 minutes)"
    make -C disasm -j"$JOBS" "${MAKE_ARGS[@]}"      # also checks that the rebuilt ROM equals yours (md5sum -c)
fi
[ -f disasm/build/pokestadium-us.elf ] || die "pret's build did not produce disasm/build/pokestadium-us.elf"

# ---- 4. N64Recomp -----------------------------------------------------------------------------------------------------------
TOOLS="$ROOT/switch/tools"
if [ ! -f "$TOOLS/N64Recomp.exe" ] || [ ! -f "$TOOLS/RSPRecomp.exe" ]; then
    mkdir -p "$TOOLS"
    if [ -e "$ROOT/lib/N64ModernRuntime/N64Recomp/.git" ]; then
        SRC="$ROOT/lib/N64ModernRuntime/N64Recomp"        # already fetched by switch/setup_deps.sh
    else
        SRC="$TOOLS/src"
        [ -d "$SRC/.git" ] || git clone -q "$N64RECOMP_REPO" "$SRC"
    fi
    git -C "$SRC" checkout -q "$N64RECOMP_COMMIT"
    for sub in lib/rabbitizer lib/ELFIO lib/fmt lib/tomlplusplus lib/sljit; do
        git -C "$SRC" submodule update -q --init --recursive "$sub"
    done
    say "building the N64Recomp tool with MinGW-w64 gcc"
    # The MinGW compiler and CMake are normal Windows programs (not MSYS ones), so they get Windows-style paths (cygpath -m)
    # and their own folder first on PATH. -static puts the C++ runtime inside the .exe so it runs from any shell.
    (
        export PATH="/mingw64/bin:$PATH"
        cmake -S "$(cygpath -m "$SRC")" -B "$(cygpath -m "$TOOLS/build")" -G Ninja -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_EXE_LINKER_FLAGS=-static >/dev/null
        cmake --build "$(cygpath -m "$TOOLS/build")" --target N64RecompCLI RSPRecomp
    )
    cp "$TOOLS/build/N64Recomp.exe" "$TOOLS/N64Recomp.exe"
    cp "$TOOLS/build/RSPRecomp.exe" "$TOOLS/RSPRecomp.exe"
fi

# ---- 5. recompile -----------------------------------------------------------------------------------------------------------
if [ ! -f generated/lookup.cpp ] || [ "${PSR_REGEN:-0}" = 1 ]; then
    say "recompiling the game to C"
    rm -rf generated
    mkdir -p generated
    "$TOOLS/N64Recomp.exe" game.toml
fi

# ---- 6. smaller call sites ---------------------------------------------------------------------------------------------------
# The same step as in codegen.sh. The files are given to Python in batches because Windows limits a command line to about
# 32,000 characters. The script is idempotent and handles each file on its own, so batching changes nothing.
if [ "${PSR_COMPACT_CALLS:-1}" = 1 ]; then
    say "shrinking call sites"
    ls generated/funcs_*.c | xargs -n 100 "$OSAKA_PY" -I switch/tools/compact_calls.py
fi
# ---- 7. the game's RSP microcode ---------------------------------------------------------------------------------------
# Same step as in codegen.sh (rsp/*.cpp are made from your ROM, not stored in the repository).
"$ROOT/switch/tools/gen_rsp.sh" "$TOOLS/RSPRecomp.exe" "$OSAKA_PY"
for f in generated/lookup.cpp generated/recomp_overlays.inl; do
    [ -f "$f" ] || die "N64Recomp did not produce $f"
done
say "Generated $(ls generated/*.c | wc -l) C files in generated/"
