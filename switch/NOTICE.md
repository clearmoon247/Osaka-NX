# Notices: licences, credits and legal notes for Osaka-NX

This file lists, component by component, where code and artwork come from and under what terms. It is written for people who
want to check the project before using or sharing it. It is a plain-language summary by the project's authors, **not legal
advice**; the licence texts themselves are authoritative.

## 1. What this project is, legally

Osaka-NX is **derived from** [mstan/PokemonStadiumRecomp](https://github.com/mstan/PokemonStadiumRecomp) by Matthew Stanley, which is
licensed under the **GNU General Public License, version 3** (see `../COPYING`). It is a separate repository, not a GitHub fork, and
is not endorsed by upstream; `../README.md` explains why. Everything outside the `switch/` folder comes from upstream (minus the
removals listed in section 2) and is used under that licence, with the licence text and notices kept. The files in `switch/` are new
work (or adapted from the projects in section 3) and are offered under the same licence, GPL-3.0, unless a file's own header
says otherwise. Who worked on what is in `../CREDITS.md`.

## 2. Game, trademarks and no affiliation

- Osaka-NX is an independent hobby project. **It is not affiliated with, authorised by, sponsored by or endorsed by Nintendo,
  Game Freak, Creatures Inc., The Pokémon Company or any of their licensors.**
- "Nintendo", "Nintendo Switch", "Pokémon" and the names of the game and its characters are trademarks of their owners. They appear in
  this repository only to state which game the software works with and which files it reads (nominative use). The project's own
  name, "Osaka-NX", and its icon do not use any of those marks.
- **No game content is distributed.** The repository contains no ROM, no game graphics, audio or text, no game data tables, no Game
  Boy cartridge data, no copy of the game's microcode in binary form and nothing translated from the game's code. Two kinds of
  game-derived code are made on your machine from your own copy: the main recompiled game code (`generated/`) and the game's four
  small RSP programs (`rsp/aspMain.cpp`, `gbTowerMain.cpp`, `gbTowerColorMain.cpp`, `njpgdspMain.cpp`: audio, Game Boy Tower
  and JPEG), which `switch/tools/gen_rsp.sh` translates with the RSPRecomp tool during `codegen.sh`. Both are git-ignored. You need to
  supply your own legally obtained copy of the game, and the project can only be used with it.
- **What was removed from the upstream project this one is derived from.** The four `rsp/*.cpp` files above (they were stored there;
  here they are generated), the Ghidra project and extracted microcode images, saved crash reports from other people's runs, the
  Pokémon Stadium box-art image, the cartridge pictures that showed Pokémon characters and `aspmain_combined.bin` (also extracted from
  the game; `tools/build_aspmain_combined.py` makes it from your own ROM when it is needed). The small hand edits that the upstream author
  made to `aspMain.cpp` are kept as a script, `switch/tools/patch_aspmain.py`, which is applied to the freshly generated file. Removing
  files from the tree does not remove them from the upstream project's git history.
- Things the software derives from *your* copy on *your* machine (the recompiled code in `generated/` and `rsp/*.cpp`, the `.nro` you build, the
  menu pictures it reads into memory at run time, anything made by `tools/dump_stadium_ui.py`) are derived from the game and
  are not part of this project. **Do not upload or share them.**
- The small tables inside the source that describe the game (ROM offsets and checksums used to recognise the right dump, combiner
  shader key numbers in `shader_candidates.h`) are facts about file layout and numbers, not game content.

## 3. Third-party code, by component

"Fetched" means downloaded at build time by `setup_deps.sh` and not stored in this repository. "In this repository" means the
text is here.

| Component | Used for | Licence | How it is used here |
| --- | --- | --- | --- |
| [mstan/PokemonStadiumRecomp](https://github.com/mstan/PokemonStadiumRecomp) | The recompilation this project is derived from | GPL-3.0 | The base of this repository. `stadium_setup.cpp` copies a block of upstream's `src/main/main.cpp` unchanged. |
| [mstan/N64ModernRuntime](https://github.com/mstan/N64ModernRuntime) | Runtime for the recompiled game | GPL-3.0 | Fetched at a fixed revision; `patches/n64modernruntime.patch` (in this repository) is applied to the fetched copy. |
| [mstan/N64Recomp](https://github.com/mstan/N64Recomp) | The recompiler tool that turns the ROM into C | MIT | Fetched at the revision in `../n64recomp.pin`; run by `codegen.sh`; not modified. |
| [gonetz/GLideN64](https://github.com/gonetz/GLideN64) | OpenGL graphics plug-in | GPL-2.0 (its `LICENSE` says "GNU General Public License version 2") | Fetched at a fixed revision and compiled into the program; `patches/gliden64.patch` is applied to the fetched copy. |
| mupen64plus API headers (inside GLideN64) | Plug-in interface that `core_switch.cpp` implements | GPL-2.0-or-later | Used through GLideN64's copy. |
| [Trinalord777/DK64Recomp-NX](https://github.com/Trinalord777/DK64Recomp-NX) | An earlier Switch port of another recompilation, built the same way | GPL-3.0 | `mman_switch.c`, `threads_switch.c` and `crash_switch.c` started as copies of its files (renamed, then changed for this project); the structure of `main_switch.cpp`, `core_switch.cpp` and `render_gliden64.cpp` follows it. Credit to its author. |
| [libnx](https://github.com/switchbrew/libnx) | Switch system library | ISC | `window_switch.c` replaces libnx's default window code and is derived from it (`nx/source/display/default_window.c`). The library itself is linked at build time (provided by devkitPro, not stored here). |
| devkitPro (devkitA64, switch portlibs: SDL2, Mesa/nouveau, EGL) | Compiler and libraries | Various (GPL with runtime exceptions, zlib, MIT) | Installed by you or inside the `devkitpro/devkita64` Docker image; not stored here. |
| [pret/pokestadium](https://github.com/pret/pokestadium) | Decompilation used by the upstream build to describe the game's code | See that project | Upstream's `disasm` submodule. Used only as input to the build on your machine; not stored here. Offsets in `stadium_skin.cpp` and `tools/dump_stadium_ui.py` were read from its `yamls/us/rom.yaml`. |
| [MSYS2](https://www.msys2.org/) and its packages (make, git, gcc, MinGW-w64, cmake) | Unix-style shell and build tools on Windows (`build-windows-native.ps1` only) | Various (GPL, MIT, BSD) | Downloaded onto your computer by that script; not stored here. |
| [Python for Windows](https://www.python.org/) | Runs pret's tools and `compact_calls.py` on Windows (native path only) | PSF licence | Downloaded onto your computer; not stored here. |
| [n64-tools/gcc-toolchain-mips64](https://github.com/n64-tools/gcc-toolchain-mips64) (GNU binutils and gcc built for Windows) | MIPS assembler/linker for pret's build on Windows (native path only) | GPL-3.0 | Downloaded onto your computer; not stored here. |
| [decompals/ido-static-recomp](https://github.com/decompals/ido-static-recomp) release v1.2, Windows build of SGI's IDO compiler | Compiler that pret's build needs (native path only) | The release has no licence file. IDO itself is proprietary SGI software. | Downloaded onto your computer from the project's release page, the same way pret's own repository ships Linux and macOS builds of it. **This repository does not contain or redistribute it.** Whether you may use it is for you to judge. |
| Pillow (Python imaging library) | Only by `tools/make_icon.py` | HPND (permissive) | Not stored here. |
| Upstream's font (`../licenses/OFL-1.1.txt`) | Used by upstream's PC version | SIL OFL 1.1 | Not used by the Switch build. |

### Licence compatibility: do not distribute a built `.nro`

GLideN64's `LICENSE` names the GNU General Public License **version 2**, and we found no "or any later version" grant in its own source files (only some third-party files inside it carry one). The rest of the program is GPL-3.0. Code under
those two licences cannot lawfully be combined into one program that is passed on to others. For the same reason, and because the
recompiled game is derived from the ROM, **a built `osaka-nx.nro` must not be published or shared.** Sharing *source* (this repository)
and the patch files is fine, because each part stays under its own licence. Using the build yourself is fine.

If you want to publish binaries one day, replace GLideN64 with a renderer under a GPL-3-compatible licence (and still leave out the
ROM-derived code).

## 4. Original work

- **Source in `switch/`:** original except where section 3 says otherwise.
- **Icon (`icon.jpg`):** original artwork drawn only from geometric shapes by `tools/make_icon.py`, which you can read. It
  shows a castle keep, a sun and water. It uses no letters, no characters, no photographs and no imported or traced artwork,
  and does not imitate any game's or console maker's logo or colour scheme. It is released with the project under GPL-3.0; the
  authors may relicense it separately (for example under CC0) if they choose.
- **Start-up screen and cartridge picker:** the layout code is original. They imitate the *look* of a menu of the game only by
  reading that menu's font and panels from your own copy of the game at run time; none of it is stored in this repository.

## 5. Contributing and removal requests

Do not contribute ROM data, generated code, game assets, or code you do not have the right to license under GPL-3.0. If you are a
rights holder and believe something in this repository should not be here, open an issue or contact the maintainers and it will be
looked at promptly.
