# Credits

Osaka-NX exists because of the people and projects below. Licences and exactly what is used from each are in
[`switch/NOTICE.md`](switch/NOTICE.md).

## This project

- **[clearmoon247](https://github.com/clearmoon247)**: maintainer of Osaka-NX. Directed the port, supplied the hardware and the testing on a real console, and
  decided what goes into the repository.
- **Claude** (Anthropic's AI model, used through Claude Code): wrote much of the Switch port with the maintainer, including the
  libnx entry point, window, audio, input and memory layers, the cartridge picker, the build and codegen scripts, the ROM tools, the
  host tests, the documentation, and the scripts that generate the game's RSP code from the player's own ROM.

## The recompilation this is built on

- **Matthew Stanley (mstan)**: author of [PokemonStadiumRecomp](https://github.com/mstan/PokemonStadiumRecomp), the static
  recompilation of Pokémon Stadium that Osaka-NX builds on, and of the forks of N64Recomp and N64ModernRuntime it uses. Without
  that work there is nothing to port. His original README is in [`UPSTREAM_README.md`](UPSTREAM_README.md); the hand edits he made
  to the audio microcode translation are kept as `switch/tools/patch_aspmain.py`.
- **Wiseguy (Mr-Wiseguy) and the [N64Recomp](https://github.com/N64Recomp/N64Recomp) / N64ModernRuntime contributors**: the
  N64 static recompiler and runtime that the whole approach comes from.

## Code and tools used

- **[gonetz and the GLideN64 contributors](https://github.com/gonetz/GLideN64)**: the OpenGL graphics plug-in (with the mupen64plus
  API headers it contains).
- **Trinalord777, [DK64Recomp-NX](https://github.com/Trinalord777/DK64Recomp-NX)**: an earlier Switch port built the same way.
  Parts of the memory, thread and crash layers started from its files, and the program structure follows it.
- **[libnx](https://github.com/switchbrew/libnx) and [devkitPro](https://devkitpro.org/)** (switchbrew, fincs, WinterMute and
  contributors): the Switch system library, compiler and portlibs. `window_switch.c` is derived from libnx.
- **[pret/pokestadium](https://github.com/pret/pokestadium) contributors**: the decompilation whose build describes the game's code
  to the recompiler, and whose ROM layout data some tools here read.
- **[decompals/ido-static-recomp](https://github.com/decompals/ido-static-recomp)**, **MSYS2**, **Python**,
  **[n64-tools/gcc-toolchain-mips64](https://github.com/n64-tools/gcc-toolchain-mips64)**: build tools used only by the optional
  native-Windows build path; downloaded, never stored here.

If your work is used here and you are not listed, or you would like your entry changed, please open an issue and it will be fixed.
