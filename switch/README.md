# Osaka-NX

A Nintendo Switch homebrew port of the [PokemonStadiumRecomp](https://github.com/mstan/PokemonStadiumRecomp) static
recompilation by Matthew Stanley (Osaka-NX is derived from it, not a GitHub fork; see [`../CREDITS.md`](../CREDITS.md)). You supply your own copy of the game;
Osaka-NX supplies the Switch side: window, graphics, audio, input, memory, a cartridge picker for the Transfer Pak, and the
build scripts.

> **Not affiliated with or endorsed by Nintendo, Game Freak, Creatures Inc. or The Pokémon Company.** Game and company
> names are used only to say which game the project works with. See [`NOTICE.md`](NOTICE.md) for licences and legal notes.

## Contents

1. [What is and is not in this repository](#what-is-and-is-not-in-this-repository)
2. [Status](#status)
3. [Requirements](#requirements)
4. [Build](#build)
5. [Install and first start](#install-and-first-start)
6. [Settings (`osaka-nx.cfg`)](#settings-osaka-nxcfg)
7. [Cartridge picker](#cartridge-picker)
8. [Start-up screen and shader warm-up](#start-up-screen-and-shader-warm-up)
9. [Logs and troubleshooting](#logs-and-troubleshooting)
10. [How it works (code map)](#how-it-works-code-map)
11. [Known issues](#known-issues)
12. [Tests and tools](#tests-and-tools)

## What is and is not in this repository

**In it:** source code for the Switch layer (`switch/`), two small patch files for third-party libraries
(`switch/patches/`, explained line by line in [`switch/patches/README.md`](patches/README.md)), build scripts, and an original icon.

**Never in it, and never to be added:**

- the game ROM, in any form (the `.gitignore` blocks `*.z64`, `*.n64`, `*.v64`),
- anything generated from the ROM: the recompiled code (`generated/`), the `.nro` you build, UI dumps made by
  `tools/dump_stadium_ui.py`,
- Game Boy ROMs or saves.

The recompiled game and the `.nro` are derived from the ROM, so they are for your own use only. Do not upload them.
Also do not publish a built `.nro` for a licensing reason that has nothing to do with the ROM: GLideN64 is GPL-2.0-only and
the rest is GPL-3.0, which cannot be combined in one distributed binary (see `NOTICE.md`). Source and patches are fine to share.

The menu pictures of the cartridge picker (font, panels, background) are read from *your* ROM at run time and kept in memory
only. Without the ROM it falls back to plain text.

## Status

| Area | State |
| --- | --- |
| Boot, menus, audio, battles, Stadium-mode tournaments | Verified on a console (firmware 21.2.0, Atmosphère 1.10.2). |
| Game Boy Tower / Transfer Pak | Verified on a console. |
| Menu jitter, transition stutter, title-to-Game-Pak glitch, pick-team glitch, knock-out freezes | Fixed and verified on a console. |
| Shader warm-up (no freezes when an effect appears for the first time) | Verified; costs about 25 s at every start. |
| Shorter warm-up that learns which shaders you use, and the start-up screen | Written; **needs confirmation on a console** before a first release. |
| Rename to Osaka-NX (folder, file and window names, data-folder migration) | Written; **needs confirmation on a console** before a first release. |

Only the first player port and Transfer Pak paths have been played extensively; other minigames and four-player play have had
little testing. Joy-Con pairing and handheld behaviour is only verified for one player.

## Requirements

- A Switch running Atmosphère custom firmware (tested: Atmosphère 1.10.2, firmware 21.2.0).
- **Title takeover.** Start the homebrew by holding **R** while launching an installed game, then choosing it. Applet mode (launching
  from the album) leaves far less memory than this needs.
- Your own **Pokémon Stadium (USA) v1.0** ROM, `.z64`, `.n64` or `.v64` (MD5 `ed1378bc12115f71209a77844965ba50`).
- For the Transfer Pak / GB Tower: your own Game Boy ROMs and saves of Red, Blue or Yellow.
- To build: a PC with Windows + WSL2 (Ubuntu 24.04) or a Linux machine, about 15 GB free disk, Docker or devkitPro (see below).

## Build

Everything runs on your own machine; nothing is uploaded anywhere (apart from downloading tools and sources). The build needs about
15-20 GB of disk and, for a first build, roughly 1-2 hours. You need the repository as a **git clone** (`git clone --recursive <this
repository's URL>`), not a ZIP download, because the build fetches a submodule.

### Easiest way: one command

**Linux (Ubuntu 22.04 or newer, x86_64)**

```
cd osaka-nx
switch/build-nro.sh ~/Downloads/Pokemon_Stadium__USA_.z64 --install
```

**Windows 10/11** - double-click `switch\build-windows.bat`. It installs WSL2 and Ubuntu 24.04 if needed (Windows may ask for
administrator rights and a restart; run it again afterwards), opens a file picker for your ROM, runs the same build inside Ubuntu, and
puts `osaka-nx.nro` in an `Osaka-NX` folder on your Desktop. If you have 8 GB of RAM or less, read the memory note it prints.

What `switch/build-nro.sh` does, in order:

1. Checks that your ROM is the right game (any of the `.z64`, `.v64`, `.n64` byte orders; it converts as needed). A wrong file is
   reported in seconds, before anything is installed. With no ROM argument it looks in the repository, your home folder, `Downloads`, `Desktop` and `Documents`.
2. Checks the computer: system, free disk, memory, needed programs, internet access, and a Switch compiler (devkitPro or Docker).
3. With `--install` (or after you answer "y"), installs what is missing: Ubuntu packages through `apt`, and devkitPro with the Switch
   libraries. Each installation step is printed before it runs. It asks for your password through `sudo`.
4. Runs the three build stages below, then tells you where `osaka-nx.nro` is and what to copy where.
5. If something fails it writes `osaka-nx-build-report.txt` (system facts and the end of the failing log; no ROM data; your home folder path is replaced by `~`)
   so you can attach one file to a bug report.

Options: `--check` (look and report only, change nothing), `--install` (agree to all installations), `--yes`, `--fast` (compile quicker; the
program is bigger and slower), `--toolchain devkitpro|docker|auto`, `--jobs N`, `--out FOLDER` (copy the `.nro` there), `--help`.
You can run the same command again after any failure: finished stages are skipped.

### Windows without WSL (experimental, never run yet)

`switch\build-windows-native.bat` builds with Windows programs only: no WSL, no virtual machine. It installs, under `C:\OsakaNX`
and without administrator rights, MSYS2 (a small Unix-style shell), Python 3.12, MIPS binutils for Windows and the Windows build of
the IDO compiler, all downloaded from their own projects and checked (SHA-256 for the files, the publisher's signature for Python).
It then runs the same stages inside MSYS2: `switch/setup_deps.sh`, `switch/codegen-msys.sh` (the Windows version of `codegen.sh`) and
`switch/build.sh`. The last stage needs devkitPro for Windows with "Switch development"; if it is missing the script stops after
making `generated/` and tells you what to install.

Why this is flagged experimental: pret's decompilation says native Windows is unsupported, and this path was written without a
Windows machine to run it on. It has a built-in safety check - pret's build compares the ROM it rebuilt with yours byte for byte -
so a tool that behaved differently from the Linux one stops the build instead of producing a wrong game. When something fails the
script saves `osaka-nx-native-report.txt` (Windows version, memory, disk, failing stage, end of the log, no ROM data) in the output
folder. Options: `-Rom`, `-OutDir`, `-Root`, `-Fast`, `-CheckOnly`, `-SkipHashCheck`. Expect roughly 2 to 4 hours the first time.

### Step by step (what the one-command build wraps)

```
# 1. Once: packages.
sudo apt update && sudo apt install -y build-essential git cmake ninja-build binutils-mips-linux-gnu python3 python3-pip python3-venv curl

# 2. Get the source.
git clone --recursive <this repository's URL> osaka-nx && cd osaka-nx

# 3. Build everything (the ROM must be .z64; tools/rom_tool.py convert can convert a .v64 or .n64).
switch/all.sh ~/Pokemon_Stadium__USA_.z64
```

`switch/all.sh` runs three stages and logs each to `logs/`; every stage skips work that is already done, so after a failure
fix the problem and run the same command again.

| Stage | Script | What it does |
| --- | --- | --- |
| 1 | `switch/setup_deps.sh` | Clones N64ModernRuntime, N64Recomp and GLideN64 at fixed revisions into `lib/` and applies the two patch files. |
| 2 | `switch/codegen.sh <rom>` | Builds the decompilation, builds the N64Recomp tool, and recompiles *your* ROM into `generated/` and translates the game's four small RSP programs into `rsp/*.cpp` (both git-ignored; the RSP step takes a second or two and runs even when `generated/` already exists). |
| 3 | `switch/build.sh` | Compiles everything for the Switch with devkitA64 (local devkitPro if `DEVKITPRO` is set, otherwise the `devkitpro/devkita64` Docker image) and produces `switch/build/osaka-nx.nro`. |

Environment variables for the build scripts (all optional):

| Variable | Effect |
| --- | --- |
| `PSR_JOBS=N` | Number of parallel compiles. The generated files are huge; too many at once gets the compiler killed for lack of memory. Default: all cores. |
| `PSR_SMALL_CODE=OS` | Compile the recompiled game with `-Os`: about 10% smaller, somewhat slower. |
| `PSR_COMPACT_CALLS=0` | Skip `tools/compact_calls.py`, which rewrites repeated call-site code in `generated/` into shared helpers (about a quarter less code). |
| `PSR_KEEP_CF_NOTE=1` | Keep the recompiler's control-flow probe in the generated code (compiled out by default to save size). |
| `PSR_GENERATED_DEBUG=1` | Keep debug info on the generated code (only makes the `.elf` bigger). |
| `PSR_REGEN=1` | Force stage 2 to regenerate `generated/` even if it exists. |
| `PSR_USE_DOCKER=1` | Use Docker even if devkitPro is installed. |

### Build time

The Switch compile of the recompiled game (`generated/`, about 90% of the program) is the longest stage. What is in place:

- **Biggest files first.** `CMakeLists.txt` lists the generated files by descending size, so the slowest compiles start at once and
  do not leave one processor busy at the end while the rest wait.
- **Per-stage times.** `build-nro.sh` and `all.sh` print how long each stage took, so any change can be compared.
- **Huge files one at a time, the rest in parallel.** The largest generated files (over 6 MB of C; the biggest are about 19 MB) are
  compiled in libraries of their own, one (`PSR_BIG_LANES`, default 1) at a time, next to everything else. That is what makes a
  higher job count safe. `PSR_BIG_FILE_BYTES` changes the size limit; both are read on every configure.
- **Parallel jobs.** From the free memory: 2.5 GB reserved for the big file, then 1.2 GB per compile at `-Os` or 1.8 GB at `-O2`,
  up to the number of processors (`build-nro.sh` and `build.sh` use the same rule; before, it was one compile per 3 GB). The
  figures come from one user's measurement (175-380 MB per compile at `-Os`, up to 975 MB at `-O2`, for files of 0.7-2.7 MB) and
  are extrapolated for larger files, so they are on the safe side but not proven. Override with `--jobs N` or `PSR_JOBS=N`; if the
  compiler is killed for lack of memory, run the same command again with fewer jobs (finished files are kept).
- **`-Os` compiles much faster than `-O2`.** `build-nro.sh` builds with `-Os` unless you pass `--fast`; `switch/all.sh` alone uses
  `-O2` unless `PSR_SMALL_CODE=OS` is set. One 2.7 MB file took 353 s at `-O2` against about 18 s at `-Os` in the measurement, and
  other files 12-20% longer at `-O2`. `-Os` is a little slower at run time; compare on a console before choosing.
- **ccache (optional).** `build-nro.sh --ccache` (or `PSR_CCACHE=1`, needs `sudo apt install ccache`; local devkitPro only, not the Docker route) keeps compiled files. The first
  build is not faster; a rebuild from scratch afterwards takes minutes.
- **Do not change settings between runs.** Switching `--fast` on and off, or any `PSR_*` compile variable, recompiles every
  generated file.
- **Work in the Linux file system** (your WSL home folder), not under `/mnt/c`.

`switch/tools/compile_time_experiment.sh` compiles a few of your generated files under several compiler settings and writes
`build/compile_time_experiment.txt` with seconds, peak memory and code size for each, plus the slowest compiler passes. Use it before
changing any compiler flag; the flags in `CMakeLists.txt` were chosen for size and run-time speed, not compile time.

If a build fails, the last lines of the failing log (`logs/…` or `switch/build/build.log`) are what to look at.

## Install and first start

1. Copy `switch/build/osaka-nx.nro` to `sdmc:/switch/`.
2. Create `sdmc:/switch/osaka-nx/` and put your Pokémon Stadium ROM in it. (The first `.z64`, `.n64` or `.v64` there that
   matches the known USA dump is used.)
3. Launch by title takeover (hold **R** while starting a game, then pick Osaka-NX).
4. **Upgrading from the earlier pre-release build?** Its data folder was `sdmc:/switch/pokestadiumrecomp/`. On start the app moves
   everything it finds there into `sdmc:/switch/osaka-nx/` (ROM, `saves/`, `launcher.cfg`, learned shader lists), renames
   `psrnx.cfg` to `osaka-nx.cfg`, and fixes absolute paths in `launcher.cfg`. If the new folder already exists (it does as soon as
   you put the `.nro` there) the two are merged: only items missing from the new folder are moved, a file that exists in both
   places is never replaced (the new one is used and the old one stays), and the old `.nro` is not moved. The log says what
   happened (`data folder: …`). Saves live in `sdmc:/switch/osaka-nx/saves/`; if one still does not show up, compare that folder
   with the old `saves/` folder.

Controls follow the Nintendo 64 layout as on Nintendo Switch Online: A→A, B→B, ZL→Z, L→L, R/ZR→R, +→Start, right stick→C buttons,
D-pad→D-pad, left stick→analog stick. Stadium's menus read only the D-pad, so the left stick also presses the D-pad (`left_stick_dpad`).

## Settings (`osaka-nx.cfg`)

An optional text file in the data folder: one `key=value` per line, `#` starts a comment. A missing file or key means the default.
The reader is `psrnx_config.cpp`; the meaning of each value is documented next to its field in `psrnx.h`.

| Key | Default | Meaning |
| --- | --- | --- |
| `resolution_factor` | `2` | Render resolution: 1 = 320×240, 2 = 640×480, 3 = 960×720. |
| `widescreen` | `2` | `2` stretch everything to 16:9; `1` widen 3D scenes only (experimental, menus can glitch); `0` 4:3 with black bars. |
| `audio_buffer_frames` | `1024` | SDL audio buffer size (256–4096). Smaller values starve the audio on this platform. |
| `left_stick_dpad` | `1` | The left stick also presses the D-pad (menus). |
| `nintendo_buttons` | `1` | Switch A confirms and Switch B cancels, as printed on the pad. `0` = by position. |
| `gb_view` | `2` | Game Boy Tower screen: `0` as drawn, `1` centred in Game Boy proportions, `2` stretched to fill, `3` in a centred 4:3 box. |
| `gb_picker` | `auto` | Cartridge picker: `auto`, `always` or `never`. |
| `picker_wait_ms` | `1500` | How long the start-up prompt for the picker waits (0 = no prompt). |
| `splash` | `1` | Show the "Please wait" screen with a progress bar during the shader warm-up. |
| `shader_warm_ms` | `45000` | Time budget for building shaders at start-up (0 = off; shaders then build on demand and freeze the game each time). |
| `shader_warm_extra` | `1` | Also build shaders a scan of the game says it *may* use: 0 none, 1 likely, 2 all. |
| `shader_prune` | `1` | Once 40 used shaders are recorded, build only those at start-up (`2` = also the built-in list, `0` = always the full list). |
| `shader_opt` | `1` | Shader compiler optimisation level 0–3 (`-1` = driver default). Lower is faster to start, slightly slower to run. |
| `vi_hold` | `150` | After a video-mode change, hide unrendered pictures for at most this many frames. |
| `vi_field` | `1` | Which field of the 480i menus is shown (1 upper, 2 lower, 0 alternate = jitter). |
| `gl_copy_color` | `0` | GLideN64 colour-to-RDRAM copy mode (-1 = GLideN64 default). 0 is much faster on page loads. |
| `gl_swap_mode` | `-1` | GLideN64 buffer swap mode (-1 = default). |
| `stall_boost` | `1` | Ask the OS for the CPU boost clock during a loading stall. |
| `log_buffered` | `1` | Log files are written through memory buffers (avoids a stall when the runtime prints hundreds of lines). |
| `interp_discovery` | `1` | Record functions the static recompile lacks in `coverage/runtime_captures.json`. |
| `hash_miss_dump` | `0` | Let the runtime write `hash_miss_id_EF.bin` at an unknown overlay (a stall of about 0.3 s each). |
| `gbcart_ring` | `0` | Write `gbcart_ring.txt`, the Transfer Pak access log (freezes the game while it is written; diagnosis only). |
| `vi_probe` | `0` | Diagnosis: log how the presented picture moves. Slows the game. |

## Cartridge picker

For the Transfer Pak / GB Tower the game needs Game Boy cartridge files assigned to controller ports. Before the game starts, a screen
can scan the SD card for Game Boy ROMs and `.sav` files and let you assign one cartridge per port. It scans the data folder
(3 levels), `sdmc:/roms` (4 levels) and the whole card (3 levels, including `/switch`, skipping only system data such as `nintendo`,
`atmosphere`, `emummc` and app caches; the list is `default_skip()` in `gbpicker.cpp`), recognises Generation-1 ROMs from
their header, checks saves by checksum, shows the trainer name and ID, and pairs a ROM with a save of the same name. The result is
written to `launcher.cfg` (the previous file is kept as `launcher.cfg.bak`; a hand-written example is `launcher.cfg.example`).

- It opens by itself on the first start when ROMs were found. After that, each start shows a short prompt: press or hold
  **L, R, ZL, ZR or −** while it is up to open the picker, otherwise the game starts.
- Keys: Up/Down (or Left/Right) choose a port, **A** choose cartridge then save, **X** change the save only, **Y** clear the port,
  **+** save and start, **B** or **−** start without changes.
- Look: with your Stadium ROM in the data folder it is drawn in the style of the game's own menu (see *What is and is not in this
  repository*); otherwise it is a plain text screen.

## Start-up screen and shader warm-up

The Switch's graphics driver takes 100–500 ms to build each shader and has no cache between runs, so the first time an effect
appeared in a battle the game froze. To avoid that, the shaders are built **before the game starts**; this takes 20+ seconds,
during which the screen shows a "Please wait" picture in the style of the game's menus with a progress bar (`splash=1`). The game
records which shaders it really draws with (`shader_used.txt`); once 40 are known, later starts build only those, so start-up shrinks
as you play. A shader the list did not contain still builds once on demand, and is remembered.

## Logs and troubleshooting

Everything is in the data folder (`sdmc:/switch/osaka-nx/`), or on the network when started with `nxlink -s`:

| File | Content |
| --- | --- |
| `osaka-nx.log`, `osaka-nx.prev.log` | Log of this run and the previous run. Lines from this port start with `[PSRNX]`. |
| `osaka-nx.stderr.log` | The runtime's error output. |
| `shader_used.txt`, `shader_keys.txt`, `shader_warm.txt`, `shader_info.txt` | Shader lists and timing, kept between runs. Safe to delete (the warm-up then starts from its built-in list). |
| `vi_ring.txt`, `vi_trans_N.txt` | Video-mode diagnostics, rewritten while playing. |
| `last_error.log`, `coverage/runtime_captures.json` | Written by the runtime. |

Useful log lines: `shader warm-up:` (how long start-up took), `splash:` (the start-up screen), `SLOW ProcessDList` (a slow display list),
`prof end: stalled N ms` (the game paused for N ms), `data folder:` (migration). `tools/symbolize.sh` turns the stall sampler's addresses
into function names (needs the `.elf` from `switch/build/`).

## How it works (code map)

Start-up order: `main()` in `main_switch.cpp` → move the old data folder if needed (`data_migration.h`) → cartridge picker (before any window exists) →
open the log → register the game (`stadium_setup.cpp`) → create the window and the GLideN64 renderer (`render_gliden64.cpp`, which
builds shaders behind the start-up screen) → start the recompiled game.

| File | Role |
| --- | --- |
| `main_switch.cpp` | Entry point: boot sequence, logging, data-folder migration, heartbeat and stall sampler, scheduler watchdog. |
| `psrnx.h`, `psrnx_config.cpp` | Settings structure and the reader for `osaka-nx.cfg`. |
| `data_migration.h` | The one-time rename of the old data folder (header-only, tested by `tests/migrate_test.cpp`). |
| `stadium_setup.cpp` | Game registration (overlays, RSP hooks): copied verbatim from upstream's `main.cpp`. |
| `render_gliden64.cpp` | Connects GLideN64 to the runtime; shader warm-up lists and learning; start-up screen; Game Boy Tower view; video-mode hold. |
| `core_switch.cpp` | A minimal stand-in for the mupen64plus "core" that GLideN64 expects (config store, window, buffer swap). |
| `gliden64_sources.cmake`, `gliden64/Revision.h` | The list of GLideN64 sources to compile, and its revision string. |
| `audio_switch.cpp` | Audio output (the path upstream uses by default, without the Windows parts). |
| `input_switch.cpp`, `stick_dpad.h` | Controllers for up to four players; the left-stick-as-D-pad logic. |
| `rsp_select.cpp` | Chooses the RSP microcode implementation for each task. |
| `runtime_hooks_switch.cpp` | Functions the runtime expects the application to provide (on PC they live in the debug server). |
| `gbpicker.h/.cpp` | Portable picker logic: scanning, matching, `launcher.cfg` handling, screen flow. |
| `gbpicker_gfx.h/.cpp`, `stadium_skin.h/.cpp` | Drawing the picker and the start-up screen; reading the font and panels from your ROM. |
| `gbpicker_switch.cpp` | The picker's Switch front end (pads, framebuffer, text console fallback). |
| `gbtower_dump_switch.c` | Text dump of the Game Boy emulator's state, for the Game Boy Tower watcher. |
| `mman_switch.c`, `threads_switch.c`, `crash_switch.c`, `window_switch.c` | Memory, threads (names choose the core; bigger stacks), crash report, window and vsync. |
| `call_post_switch.c`, `shim_generated/recomp.h` | Shared helpers and header overrides used only by the generated game code (smaller build). |
| `shim/` | Stub headers (`dlfcn.h`, `sys/mman.h`) because Horizon has no dynamic loading or `mmap`. |
| `shader_candidates.h` | Machine-written list of shader keys the game may use (numbers only; made by `tools/shader_candidates/`). |
| `psrnx_sample.h` | Record shared by the stall sampler's C and C++ parts. |
| `CMakeLists.txt` | The Switch build. `icon.jpg` is the application icon. |
| `build-nro.sh` | The one-command build: checks, installs, finds and converts the ROM, runs the stages, writes the failure report. |
| `build-windows.bat`, `build-windows.ps1` | Windows launcher: sets up WSL2 + Ubuntu, copies the repository and ROM in, runs `build-nro.sh`, copies the `.nro` out. |
| `build-windows-native.bat`, `build-windows-native.ps1`, `codegen-msys.sh` | Experimental Windows build without WSL: installs MSYS2, Python, MIPS binutils and the Windows IDO compiler under `C:\OsakaNX`, then runs the stages in MSYS2. `codegen-msys.sh` is the MSYS2 version of `codegen.sh`. |
| `all.sh`, `setup_deps.sh`, `codegen.sh`, `build.sh`, `sizes.sh`, `symbolize.*` | The build stages and diagnostic scripts (see *Build*). |
| `patches/` | Patches for N64ModernRuntime and GLideN64; see `patches/README.md`. |

Memory: the generated code masks guest addresses with `0x3FFFFFFF`, so the guest needs a 1 GiB window. Horizon has no `mmap`, so
`mman_switch.c` provides one eagerly committed 1 GiB block. This fits in title takeover alongside GLideN64, Mesa and the ROM.

## Known issues

- **Start-up takes about 25 s** the first time (shader warm-up); later starts should be shorter once the shaders you use are recorded.
- A few screen loads still run slow display lists (190–560 ms).
- Upstream recompilation bugs are not fixed here: the Rental roster crash (upstream issue 13) and announcer static (issue 10).
- The Transfer Pak needs Generation-1 cartridge files (Red, Blue, Yellow, Japanese Green).
- Exiting returns to the Home menu rather than hbmenu.
- The `.nro` is large (about 200 MB with `PSR_SMALL_CODE=OS`).

## Tests and tools

`switch/tests/run.sh` builds and runs the host tests (Linux/WSL, no console needed): the cartridge picker logic, the left-stick mapping, the
data-folder migration, the ROM finder/converter and the call-site helpers. With `STADIUM_DUMP=<folder>` (made by `tools/dump_stadium_ui.py` from your own ROM) it also checks the ROM reader and
renders sample screens.

| Tool | What it does |
| --- | --- |
| `tools/compile_time_experiment.sh` | Times the Switch compiler on a few of your generated files under different settings (seconds, peak memory, code size). |
| `tools/rom_tool.py` | Identifies your ROM, converts it between byte orders, searches folders for it (used by `build-nro.sh`; tested by `tests/rom_tool_test.py`). |
| `tools/make_icon.py` | Draws `icon.jpg` from scratch (original artwork, see `NOTICE.md`). |
| `tools/compact_calls.py` | Shrinks the generated code (run by `codegen.sh`). |
| `tools/gen_rsp.sh` | Translates the audio, Game Boy Tower and JPEG RSP programs from your ROM with RSPRecomp into `rsp/*.cpp` (run by `codegen.sh`; `PSR_REGEN_RSP=1` forces it). |
| `tools/patch_aspmain.py` | Re-applies the upstream author's small hand edits to the generated `rsp/aspMain.cpp` (run by `gen_rsp.sh`). |
| `tools/dump_stadium_ui.py` | Dumps UI assets from your own ROM (for the tests only; private use). |
| `tools/shader_candidates/` | How `shader_candidates.h` was made. |
| `tools/size_experiment.sh`, `tools/make_sample_bundle.sh` | Size experiments on generated files. |
