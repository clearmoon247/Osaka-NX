#!/usr/bin/env bash
# build-nro.sh: the one command that turns your own Pokemon Stadium ROM into osaka-nx.nro.
#
#   switch/build-nro.sh                     look for your ROM, check this computer, ask before installing anything, build
#   switch/build-nro.sh /path/to/rom.z64    same, with the ROM named (.z64, .v64 and .n64 byte orders all work)
#   switch/build-nro.sh --check             only check this computer and the ROM, and say what is missing (changes nothing)
#
# Options (all optional):
#   --install            install missing tools without asking (needs sudo; Debian/Ubuntu only)
#   --yes                answer "yes" to every question (for scripts)
#   --toolchain WHICH    devkitpro = devkitPro on this machine (default if present), docker = the devkitpro/devkita64 image,
#                        auto = whichever is available, installing devkitPro if neither is
#   --jobs N             parallel compiles (default: chosen from your memory; each big file needs about 3 GB)
#   --ccache             remember compiled files (needs the ccache program): the first build is NOT faster, but a rebuild
#                        from scratch later takes minutes instead of hours
#   --fast               compile the recompiled game with -O2 (default is -Os: smaller .nro, a little slower)
#   --out DIR            when done, copy osaka-nx.nro into DIR (for example your SD card's switch folder)
#   -h, --help           this text
#
# Where it works: x86_64 Linux, including Ubuntu in WSL2 on Windows (see switch/build-windows.bat for the Windows launcher).
#
# What it does, in order (each part is a function below; the build stages skip work that is already finished, so after a
# failure you fix the problem and run the same command again):
#   0. check your ROM first (a wrong file is reported in seconds)      (resolve_rom)
#   1. check this computer: system, disk, memory, tools, internet, build toolchain       (check_system)
#   2. install what is missing, if you agree                                              (install_host_packages, install_devkitpro)
#   3. find your ROM, make sure it is the right game, convert it to the byte order needed (resolve_rom)
#   4. stage 1: download third-party sources and patch them     (switch/setup_deps.sh)
#      stage 2: recompile YOUR ROM into C code                  (switch/codegen.sh)   -> generated/ (stays on your machine)
#      stage 3: compile for the Switch                          (switch/build.sh)     -> switch/build/osaka-nx.nro
#   5. if anything fails: write osaka-nx-build-report.txt (system facts + the end of the failing log, no ROM data)
#
# Nothing derived from your ROM is ever uploaded or committed. The recompiled code and the .nro are for your own use only.

set -uo pipefail

# ---- Paths ---------------------------------------------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK="$REPO/.osaka-work"                    # scratch folder (git-ignored): holds the z64 copy of the ROM
REPORT="$REPO/osaka-nx-build-report.txt"
LOGS="$REPO/logs"
NRO="$REPO/switch/build/osaka-nx.nro"
ROM_TOOL="$SCRIPT_DIR/tools/rom_tool.py"
cd "$REPO"

# ---- Settings (changed by the options) --------------------------------------------------------------------------------------
ROM_ARG=""
CHECK_ONLY=0
ASSUME_YES=0
INSTALL=0
TOOLCHAIN="auto"
JOBS=""
SMALL_CODE=1
USE_CCACHE=0
OUT_DIR=""
CURRENT_STAGE="start"                       # named in the failure report
PROBLEMS=()                                 # things --check found that stop a build

# ---- Messages ------------------------------------------------------------------------------------------------------------------
say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
good() { printf '  [ok]      %s\n' "$*"; }
warn() { printf '  [warning] %s\n' "$*"; }
bad()  { printf '  [MISSING] %s\n' "$*"; PROBLEMS+=("$*"); }

usage() { sed -n "2,/^\$/p" "$SCRIPT_DIR/build-nro.sh" | sed 's/^# \{0,1\}//'; }

# Asks a yes/no question; --yes and --install answer for you. Default answer is "no".
ask_yes() {
    [ "$ASSUME_YES" = 1 ] || [ "$INSTALL" = 1 ] && return 0
    [ -t 0 ] || return 1                    # nobody to ask (piped input): do not guess
    local reply
    read -r -p "  $1 [y/N] " reply
    case "$reply" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

# ---- Arguments -----------------------------------------------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --check)      CHECK_ONLY=1 ;;
        --install)    INSTALL=1 ;;
        --yes|-y)     ASSUME_YES=1 ;;
        --fast)       SMALL_CODE=0 ;;
        --ccache)     USE_CCACHE=1 ;;
        --toolchain)  TOOLCHAIN="${2:-}"; shift ;;
        --jobs)       JOBS="${2:-}"; shift ;;
        --out)        OUT_DIR="${2:-}"; shift ;;
        -h|--help)    usage; exit 0 ;;
        -*)           say "unknown option: $1"; usage; exit 2 ;;
        *)            ROM_ARG="$1" ;;
    esac
    shift
done
case "$TOOLCHAIN" in auto|devkitpro|docker) ;; *) say "--toolchain must be auto, devkitpro or docker"; exit 2 ;; esac

# ---- Failure report ------------------------------------------------------------------------------------------------------------
# Collects what a person helping you would ask for, so you can attach one file instead of copying text around. It contains
# system facts and the end of the failing log (with your home folder name replaced by ~). It never contains ROM data.
write_report() {
    local stage_log="$1"
    {
        echo "Osaka-NX build report, $(date -u '+%Y-%m-%d %H:%M UTC'), failed in: $CURRENT_STAGE"
        echo "repository revision: $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
        echo "--- system";   uname -a; grep -m1 PRETTY_NAME /etc/os-release 2>/dev/null; grep -qi microsoft /proc/version 2>/dev/null && echo "running in WSL"
        echo "--- memory and disk"; grep -E 'MemTotal|MemAvailable|SwapTotal' /proc/meminfo; df -Pk "$REPO" | tail -1
        echo "--- tools"
        for t in git make gcc cmake ninja python3 mips-linux-gnu-ld docker dkp-pacman; do
            printf '%-18s %s\n' "$t" "$(command -v "$t" >/dev/null 2>&1 && ("$t" --version 2>&1 | head -1) || echo 'not found')"
        done
        echo "DEVKITPRO=${DEVKITPRO:-}   toolchain choice: $TOOLCHAIN   jobs: ${JOBS:-auto}"
        if [ -n "$stage_log" ] && [ -f "$stage_log" ]; then
            echo "--- last 120 lines of $stage_log"; tail -n 120 "$stage_log"
        fi
    } 2>&1 | sed "s#$HOME#~#g" > "$REPORT"
}

# Reports a failed stage and stops.
fail_stage() {
    local log="$1"
    write_report "$log"
    say ""
    say "The build stopped in: $CURRENT_STAGE"
    say "Last lines of the log:"; tail -n 15 "$log" | sed 's/^/    /'
    say ""
    say "A report for whoever is helping you was written to: $REPORT"
    say "After fixing the cause, run the same command again: finished work is not repeated."
    exit 1
}

# ---- 1. Checks -----------------------------------------------------------------------------------------------------------------
# Adds the package that provides a tool to MISSING_PACKAGES when the tool is not found. Debian/Ubuntu package names.
MISSING_PACKAGES=()
need_tool() {   # <command> <apt package>
    if command -v "$1" >/dev/null 2>&1; then good "$1"; else bad "$1 (apt package: $2)"; MISSING_PACKAGES+=("$2"); fi
}

check_system() {
    step "Checking this computer"
    CURRENT_STAGE="checking the computer"

    # System: the decompilation build runs a compiler that only exists for x86_64 Linux.
    [ "$(uname -s)" = Linux ] || { bad "this needs Linux (on Windows use switch/build-windows.bat, which sets up WSL2)"; return; }
    if [ "$(uname -m)" = x86_64 ]; then good "x86_64 Linux ($(grep -m1 PRETTY_NAME /etc/os-release 2>/dev/null | cut -d'"' -f2))"
    else bad "x86_64 processor needed (this is $(uname -m))"; fi

    # Where the repository lives: under WSL, Windows drives (/mnt/c, ...) are many times slower and break file permissions.
    case "$REPO" in
        /mnt/[a-z]/*) bad "the repository is on a Windows drive ($REPO): builds there are extremely slow. Copy it into your Linux home folder (cp -r) and run it from there" ;;
        *) good "repository is on the Linux file system" ;;
    esac

    # The build fetches the game-description submodule with git, so a "Download ZIP" copy (no .git folder) cannot work.
    if [ -e "$REPO/.git" ]; then good "repository has git history (needed for its submodules)"
    else bad "this folder is not a git clone (a ZIP download will not work): run  git clone --recursive <repository address>"; fi

    # Disk: the recompiled sources alone are over a gigabyte; about 15 GB are needed in total.
    local free_gb; free_gb=$(df -Pk "$REPO" | awk 'NR==2 {printf "%d", $4/1048576}')
    if [ "$free_gb" -ge 20 ]; then good "free disk space: ${free_gb} GB"
    elif [ "$free_gb" -ge 12 ]; then warn "free disk space: ${free_gb} GB (15-20 GB is comfortable)"
    else bad "free disk space: ${free_gb} GB (about 15 GB are needed)"; fi

    # Memory: measured peak per compile (GCC for aarch64, generated/funcs_*.c): about 175-380 MB at -Os and up to ~950 MB at -O2 for
    # files of 0.7-2.7 MB; the few files over 6 MB are compiled one at a time (CMakeLists.txt, PSR_BIG_FILE_BYTES) and are budgeted
    # separately. Larger files need more than the small ones, so the per-compile budget below is deliberately generous:
    # 1.2 GB at -Os, 1.8 GB at -O2, after 2.5 GB reserved for the big file.
    local mem_gb mem_mb per_job cpus; mem_mb=$(awk '/MemAvailable/ {printf "%d", $2/1024}' /proc/meminfo); mem_gb=$(( mem_mb / 1024 )); cpus=$(nproc)
    if [ "$SMALL_CODE" = 1 ]; then per_job=1200; else per_job=1800; fi
    if [ -z "$JOBS" ]; then
        JOBS=$(( (mem_mb - 2500) / per_job )); [ "$JOBS" -gt "$cpus" ] && JOBS=$cpus; [ "$JOBS" -lt 1 ] && JOBS=1
    fi
    if [ "$mem_gb" -ge 6 ]; then good "memory available: ${mem_gb} GB, $cpus processor threads, so $JOBS parallel compile(s)"
    else
        warn "memory available: ${mem_gb} GB: the build will use $JOBS compile at a time and may be slow."
        grep -qi microsoft /proc/version 2>/dev/null && warn "in WSL you can raise the limit: put 'memory=8GB' under [wsl2] in %UserProfile%\\.wslconfig, then run 'wsl --shutdown'"
    fi

    # Tools the recompilation steps need on this machine.
    need_tool git git;                     need_tool make build-essential;   need_tool gcc build-essential
    need_tool cmake cmake;                 need_tool ninja ninja-build;      need_tool python3 python3
    need_tool mips-linux-gnu-ld binutils-mips-linux-gnu; need_tool curl curl
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import venv, ensurepip' 2>/dev/null && good "python3 venv and pip" || { bad "python3 venv/pip (apt packages: python3-venv python3-pip)"; MISSING_PACKAGES+=(python3-venv python3-pip); }
    fi

    # Internet: the sources are downloaded from GitHub (and a few Python packages from PyPI).
    if timeout 25 git ls-remote https://github.com/mstan/N64ModernRuntime.git HEAD >/dev/null 2>&1; then good "can reach GitHub"
    else warn "could not reach GitHub from here: the download stage will fail unless a proxy or the network is fixed"; fi

    check_toolchain
}

# The Switch compiler (devkitA64 + libraries): found on this machine, in Docker, or installed.
HAVE_DEVKITPRO=0; HAVE_DOCKER=0; NEED_PORTLIBS=0
check_toolchain() {
    # devkitPro installed here? (its location is normally /opt/devkitpro, and the DEVKITPRO variable may be unset)
    [ -z "${DEVKITPRO:-}" ] && [ -d /opt/devkitpro ] && export DEVKITPRO=/opt/devkitpro
    if [ -n "${DEVKITPRO:-}" ] && [ -x "$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake" ]; then
        # Libraries the Switch build links: SDL2 and the Mesa/EGL set (package names switch-sdl2 and switch-mesa).
        local lib="$DEVKITPRO/portlibs/switch/lib"
        HAVE_DEVKITPRO=1
        if ls "$lib"/libSDL2* >/dev/null 2>&1 && ls "$lib"/libEGL* >/dev/null 2>&1 && ls "$lib"/libdrm_nouveau* >/dev/null 2>&1; then
            good "devkitPro at $DEVKITPRO (with SDL2 and Mesa)"
        else
            NEED_PORTLIBS=1; bad "devkitPro libraries SDL2 and Mesa (packages switch-sdl2 switch-mesa; this script can install them)"
        fi
    else
        say "  [info]    devkitPro is not installed on this machine"
    fi
    # Docker usable? (installed AND its background service reachable)
    if command -v docker >/dev/null 2>&1; then
        if docker info >/dev/null 2>&1; then HAVE_DOCKER=1; good "Docker is running"
        else say "  [info]    Docker is installed but not running (start Docker Desktop / the docker service to use it)"; fi
    fi

    case "$TOOLCHAIN" in
        devkitpro) [ "$HAVE_DEVKITPRO" = 1 ] || bad "devkitPro with SDL2 and Mesa (this script can install it: add --install)" ;;
        docker)    [ "$HAVE_DOCKER" = 1 ] || bad "a running Docker (or use --toolchain devkitpro)" ;;
        auto)      [ "$HAVE_DEVKITPRO" = 1 ] || [ "$HAVE_DOCKER" = 1 ] || bad "a Switch toolchain: devkitPro or Docker (this script can install devkitPro: add --install)" ;;
    esac
}

# ---- 2. Installing missing pieces ---------------------------------------------------------------------------------------------
run_root() { if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi; }

# Installs the Debian/Ubuntu packages collected by need_tool.
install_host_packages() {
    [ "${#MISSING_PACKAGES[@]}" -gt 0 ] || return 0
    local unique; unique=$(printf '%s\n' "${MISSING_PACKAGES[@]}" | sort -u | tr '\n' ' ')
    step "Installing tools: $unique"
    command -v apt-get >/dev/null 2>&1 || { say "  This is not a Debian/Ubuntu system: install these with your package manager: $unique"; return 1; }
    command -v sudo >/dev/null 2>&1 || [ "$(id -u)" = 0 ] || { say "  sudo is not available: run as root or install: $unique"; return 1; }
    ask_yes "Install these with apt (asks for your password via sudo)?" || return 1
    # shellcheck disable=SC2086
    run_root apt-get update -qq && DEBIAN_FRONTEND=noninteractive run_root apt-get install -y $unique
}

# Installs devkitPro natively, the way its own documentation says for Debian/Ubuntu: download the official installer script,
# run it (it adds devkitPro's package repository), then install the Switch packages with devkitPro's package manager.
install_devkitpro() {
    step "Installing devkitPro (the Switch compiler and libraries, about 1 GB)"
    command -v apt-get >/dev/null 2>&1 || { say "  Not a Debian/Ubuntu system: install devkitPro by hand (https://devkitpro.org/wiki/Getting_Started) or use Docker."; return 1; }
    say "  This downloads and runs the official installer https://apt.devkitpro.org/install-devkitpro-pacman as root."
    ask_yes "Do that now?" || return 1
    local installer="$WORK/install-devkitpro-pacman"
    mkdir -p "$WORK"
    curl -fsSL https://apt.devkitpro.org/install-devkitpro-pacman -o "$installer" || { say "  download failed"; return 1; }
    chmod +x "$installer"
    DEBIAN_FRONTEND=noninteractive run_root "$installer" || return 1
    # Packages: the Switch development kit, its ported libraries, and the Switch CMake wrapper the build uses.
    run_root dkp-pacman -Syu --noconfirm --needed switch-dev switch-portlibs switch-cmake switch-sdl2 switch-mesa || return 1
    export DEVKITPRO=/opt/devkitpro
    return 0
}

# devkitPro is there but SDL2 / Mesa are not: install just those packages.
install_portlibs() {
    step "Installing the devkitPro libraries SDL2 and Mesa"
    command -v dkp-pacman >/dev/null 2>&1 || { say "  dkp-pacman not found: install the packages switch-sdl2 and switch-mesa with devkitPro's package manager"; return 1; }
    ask_yes "Install switch-sdl2 and switch-mesa with dkp-pacman (uses sudo)?" || return 1
    run_root dkp-pacman -S --noconfirm --needed switch-sdl2 switch-mesa
}

# ---- 3. The ROM ---------------------------------------------------------------------------------------------------------------
ROM_Z64=""
STAGE_TIMES=()                 # one line per finished stage, printed by finish()
resolve_rom() {
    step "Your ROM"
    CURRENT_STAGE="finding the ROM"
    local rom="$ROM_ARG"
    if [ -z "$rom" ]; then
        say "  No ROM given: looking in the usual places..."
        # Common folders: this repository, home, downloads, and (under WSL) the Windows user folders.
        local places=("$REPO" "$HOME" "$HOME/Downloads" "$HOME/Desktop" "$HOME/Documents" "$PWD")
        local d; for d in /mnt/c/Users/*/Downloads /mnt/c/Users/*/Desktop /mnt/c/Users/*/Documents; do [ -d "$d" ] && places+=("$d"); done
        rom=$(python3 -I "$ROM_TOOL" find "${places[@]}") || rom=""
        [ -n "$rom" ] || { bad "no Pokemon Stadium (USA) v1.0 ROM found. Put it in your home folder or Downloads, or run:  switch/build-nro.sh /path/to/rom.z64"; return 1; }
    fi
    local verdict; verdict=$(python3 -I "$ROM_TOOL" check "$rom") && good "$verdict" || { bad "$verdict"; return 1; }
    # --check changes nothing, so it stops here.
    [ "$CHECK_ONLY" = 1 ] && return 0
    # The build wants z64 byte order: make a private copy (in the git-ignored .osaka-work folder) converted if needed.
    mkdir -p "$WORK"
    ROM_Z64="$WORK/rom.z64"
    python3 -I "$ROM_TOOL" convert "$rom" "$ROM_Z64" >/dev/null || { bad "could not prepare the ROM"; return 1; }
    good "ready for the build (copy in the z64 byte order: $ROM_Z64)"
}

# ---- 4. The build stages ------------------------------------------------------------------------------------------------------
# Runs one stage, showing it live and keeping a log in logs/. On failure writes the report and stops.
run_stage() {   # <number> <name> <command...>
    local number="$1" name="$2"; shift 2
    CURRENT_STAGE="stage $number: $name"
    local log="$LOGS/$number-$name.log" started=$SECONDS
    step "Stage $number of 3: $name"
    mkdir -p "$LOGS"
    "$@" 2>&1 | tee "$log"
    if [ "${PIPESTATUS[0]}" -ne 0 ]; then fail_stage "$log"; fi
    say "  stage $number finished in $(( (SECONDS - started) / 60 )) min $(( (SECONDS - started) % 60 )) s"
    STAGE_TIMES+=("$(printf '    stage %s %-11s %4d min %2d s' "$number" "$name" $(( (SECONDS - started) / 60 )) $(( (SECONDS - started) % 60 )))")
}

build_all() {
    export PSR_JOBS="$JOBS"
    [ "$USE_CCACHE" = 1 ] && export PSR_CCACHE=1
    [ "$SMALL_CODE" = 1 ] && export PSR_SMALL_CODE=OS || unset PSR_SMALL_CODE
    case "$TOOLCHAIN" in
        docker)    export PSR_USE_DOCKER=1 ;;
        devkitpro) unset PSR_USE_DOCKER ;;
        auto)      [ "$HAVE_DEVKITPRO" = 1 ] && unset PSR_USE_DOCKER || export PSR_USE_DOCKER=1 ;;
    esac
    say "Building with $JOBS parallel compile(s), $( [ "$SMALL_CODE" = 1 ] && echo '-Os (smaller)' || echo '-O2 (faster)' ), toolchain: $( [ "${PSR_USE_DOCKER:-0}" = 1 ] && echo Docker || echo "devkitPro at ${DEVKITPRO:-?}" )"
    run_stage 1 setup_deps  "$SCRIPT_DIR/setup_deps.sh"
    run_stage 2 codegen     "$SCRIPT_DIR/codegen.sh" "$ROM_Z64"
    run_stage 3 build       "$SCRIPT_DIR/build.sh"
    [ -f "$NRO" ] || { CURRENT_STAGE="stage 3: build (no .nro was produced)"; fail_stage "$LOGS/3-build.log"; }
}

# ---- 5. Finish ----------------------------------------------------------------------------------------------------------------
finish() {
    step "Done"
    say "  Built: $NRO ($(du -h "$NRO" | cut -f1))"
    if [ "${#STAGE_TIMES[@]}" -gt 0 ]; then say "  Where the time went (this run; skipped stages show ~0):"; printf '%s\n' "${STAGE_TIMES[@]}"; fi
    if [ -n "$OUT_DIR" ]; then
        mkdir -p "$OUT_DIR" && cp "$NRO" "$OUT_DIR/" && say "  Copied to: $OUT_DIR/osaka-nx.nro"
    fi
    cat <<'EOF'

  Next, on the Switch's SD card:
    1. Copy osaka-nx.nro into   sdmc:/switch/
    2. Create the folder        sdmc:/switch/osaka-nx/   and put your Pokemon Stadium ROM in it
    3. Start it by title takeover: hold R while launching a game, then choose Osaka-NX

  The recompiled game (generated/) and the .nro are made from your ROM: keep them to yourself.
EOF
}

# ---- Main ----------------------------------------------------------------------------------------------------------------------
# In a function so a test can load this file (source it) and call the pieces above without starting a build.
main() {
    # The ROM is checked first: a wrong file should be reported in seconds, not after tools have been installed.
    # (python3 comes with Ubuntu; if it is missing the check happens after the installation instead.)
    if [ "$CHECK_ONLY" = 0 ] && command -v python3 >/dev/null 2>&1; then
        resolve_rom || { say ""; say "Cannot continue without the right ROM (see above)."; exit 1; }
    fi
    check_system
    if [ "${#PROBLEMS[@]}" -gt 0 ] && [ "$CHECK_ONLY" = 0 ]; then
        # Try to fix what can be fixed, then check again.
        [ "${#MISSING_PACKAGES[@]}" -gt 0 ] && install_host_packages
        # devkitPro is installed when it was asked for, or when neither devkitPro nor Docker is usable.
        if [ "$HAVE_DEVKITPRO" = 0 ] && { [ "$TOOLCHAIN" = devkitpro ] || { [ "$TOOLCHAIN" = auto ] && [ "$HAVE_DOCKER" = 0 ]; }; }; then install_devkitpro; fi
        [ "$NEED_PORTLIBS" = 1 ] && install_portlibs
        PROBLEMS=(); MISSING_PACKAGES=(); NEED_PORTLIBS=0
        check_system >/dev/null 2>&1                      # quiet re-check; the next lines print what is still wrong
        [ "${#PROBLEMS[@]}" -gt 0 ] && { step "Still missing"; printf '  - %s\n' "${PROBLEMS[@]}"; exit 1; }
    fi

    if [ "$CHECK_ONLY" = 1 ]; then
        resolve_rom
        step "Result"
        if [ "${#PROBLEMS[@]}" -eq 0 ]; then say "  Everything is ready. Run without --check to build."; exit 0; fi
        say "  Not ready yet. Missing:"; printf '    - %s\n' "${PROBLEMS[@]}"
        say "  Run again with --install to let this script install what it can."
        exit 1
    fi

    [ -n "$ROM_Z64" ] || resolve_rom || { say ""; say "Cannot continue without the right ROM (see above)."; exit 1; }
    START=$SECONDS
    build_all
    finish
    say "  Total time: $(( (SECONDS - START) / 60 )) min"
}

[ "${BASH_SOURCE[0]}" = "$0" ] && main
