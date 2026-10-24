# Osaka-NX

A **Nintendo Switch homebrew port of Pokémon Stadium (US v1.0)**. You supply your own legally obtained copy of the game; Osaka-NX
supplies the Switch side (window, graphics, audio, input, memory, a cartridge picker for the Transfer Pak / Game Boy Tower) and the
scripts that turn your copy into a program you build and run on your own console.

Osaka-NX is built on [PokemonStadiumRecomp](https://github.com/mstan/PokemonStadiumRecomp) by **Matthew Stanley (mstan)**, a static
recompilation of the game made with N64Recomp. That project did the hard part: recompiling the game and running it at all. Its
original README is kept in [`UPSTREAM_README.md`](UPSTREAM_README.md). Everyone whose work this stands on is listed in
[`CREDITS.md`](CREDITS.md).

> **Not affiliated with or endorsed by Nintendo, Game Freak, Creatures Inc. or The Pokémon Company.** Game and company names are
> used only to say which game the project works with.

## What is and is not in this repository

- **Not included, ever:** the game (ROM), anything extracted or generated from it, and built programs (`.nro`). You make those
  yourself, on your own machine, from your own copy. The two kinds of game-derived code the build needs, `generated/` and
  `rsp/*.cpp`, are produced from your ROM by `switch/codegen.sh` and are git-ignored.
- **Included:** source code, build scripts, two small patches applied to third-party libraries at build time, documentation and an
  original icon. All of it can be shared under open-source terms (see below).
- Please do not share built `.nro` files or generated code, and do not attach ROMs or ROM-derived files to issues. A built `.nro`
  contains the recompiled game, and it also links GLideN64 (GPL-2.0) with GPL-3.0 code, which cannot lawfully be passed on in
  binary form. See [`switch/NOTICE.md`](switch/NOTICE.md).

## Build it

Requirements: a Pokémon Stadium (US, v1.0) ROM you own, an x86-64 Linux machine or Windows with WSL2 (Ubuntu 24.04),
15-20 GB of free disk and roughly 1-2 hours for the first build. From the cloned repository:

```
git clone --recursive <this repository's URL> osaka-nx && cd osaka-nx
switch/all.sh /path/to/your/Pokemon_Stadium_US.z64
```

That produces `switch/build/osaka-nx.nro`. [`switch/README.md`](switch/README.md) has everything else: the guided
one-command build (`switch/build-nro.sh`, `switch/build-windows.bat`), installing on the console, settings, troubleshooting, and
how the pieces fit together.

## Why this is not a GitHub fork

Osaka-NX is a separate repository on purpose. Credit to upstream is given in the documentation and by keeping its licence, not by
sharing its git history. In short:

1. **History we do not want to carry.** Upstream's history contains material that does not belong in a project meant to be free of
   game content: translated game code, extracted microcode images, other people's crash reports and official artwork. We removed
   that from the files. A fork cannot drop history, so it would keep all of it reachable.
2. **Different scope.** This project targets one platform and one way of building (bring your own ROM, generate everything
   game-derived locally). Upstream is a PC project with its own releases, launcher and plans. The two will diverge.
3. **No implied relationship.** Upstream's author has said the project is no longer maintained. A fork presents itself as a
   downstream copy that tracks and feeds back to the original; this is a different, independent project, and nothing here is
   endorsed by or speaks for upstream.
4. **A clean place for a clear licence story.** Everything here is either GPL-3.0 or fetched at build time under its own licence,
   with one history that was prepared for that purpose.

Upstream's code is GPL-3.0, which allows this as long as the licence text and copyright notices stay with the code, and they do.
This is a plain-language explanation, not legal advice.

## Licence

GPL-3.0. The full text is in [`COPYING`](COPYING). Code taken from upstream stays under its licence; new files in `switch/` are
GPL-3.0 unless a file says otherwise. Third-party components and their licences are listed in
[`switch/NOTICE.md`](switch/NOTICE.md). Use of the game itself is up to the licence of your copy; this project grants no rights to it.

## Credits

See [`CREDITS.md`](CREDITS.md). In particular: Matthew Stanley for the recompilation this builds on, the N64Recomp and
N64ModernRuntime authors, the authors of GLideN64, libnx and the Pokémon Stadium decompilation, and Claude (Anthropic's AI model),
which wrote much of the Switch port with the project's maintainer.
