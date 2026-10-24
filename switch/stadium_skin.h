// Graphics for the cartridge picker, read from the player's own Pokemon Stadium (USA) ROM at start-up so the screen can look
// like the game's "Game Pak Check" menu: the Stadium font, the rounded-panel corners, the coloured Game Boy cartridge icons
// and the repeating logo tile of the menu background. Nothing here is stored in the repository or the built program; if the ROM is missing or is not
// the known USA dump, `Skin::ok` stays false and the picker falls back to its plain text screen.
//
// Asset locations and formats come from the pret/pokestadium decompilation (yamls/us/rom.yaml, src/fragments/33).
// Portable C++17, no libnx: tests/ drives it with assets decoded from a dump.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace skin {
    // Straight (non-premultiplied) alpha, bytes R,G,B,A in memory (a little-endian uint32 is 0xAABBGGRR).
    struct Image {
        int w = 0, h = 0;
        std::vector<uint32_t> px;
        bool empty() const { return px.empty(); }
    };

    constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return static_cast<uint32_t>(r) | static_cast<uint32_t>(g) << 8 | static_cast<uint32_t>(b) << 16 | static_cast<uint32_t>(a) << 24;
    }

    // The menu font ("fonts" asset, file 3): 145 glyph tiles of 24x20, one byte per texel, high nibble = intensity (white
    // core, dark rim), low nibble = alpha. Advance widths are the 0x90-byte table at the start of the file.
    struct Font {
        static constexpr int kCellW = 24, kCellH = 20, kTiles = 145;
        std::vector<uint8_t> tiles;
        uint8_t widths[0x90] = {};
        bool ok() const { return tiles.size() >= static_cast<size_t>(kTiles) * kCellW * kCellH; }
    };

    enum CartColor { kCartRed = 0, kCartGreen = 1, kCartBlue = 2, kCartYellow = 3 };

    struct Skin {
        bool ok = false;
        std::string error;                 // why ok is false
        Image logo_tile;                   // 128x64 I4: the logo that repeats over the menu background (intensity in RGB)
        Image corner[4];                   // 16x16 alpha masks: top-left, top-right, bottom-left, bottom-right
        Image cart[4];                     // 64x64 RGBA, CartColor order
        Image label[4];                    // 32x24 "1P".."4P"
        Image arrow;                       // 48x16 red/yellow selection arrow, points up
        Font font;
    };

    // Decoded asset files (what the game holds in RAM after loading them).
    struct Sources {
        std::vector<uint8_t> font;         // fonts, file 3
        std::vector<uint8_t> menu2;        // common_menu2_ui (segment 2)
        std::vector<uint8_t> gbpak;        // gb_pak_select_ui (segment 3)
        std::vector<uint8_t> menusel;      // menu_select_ui (segment 3 of the Game Pak Check screen): the background tile
    };

    bool build(const Sources& src, Skin& out, std::string& error);

    // Reads and decompresses the assets from a USA ROM (.z64, .n64 or .v64 byte order); checks the header CRCs first.
    bool read_rom_sources(const std::filesystem::path& rom, Sources& out, std::string& error);
    bool load_from_rom(const std::filesystem::path& rom, Skin& out);
    // First .z64/.n64/.v64 in `dir` that is the USA ROM; on failure `out.error` says why.
    bool load_from_folder(const std::filesystem::path& dir, Skin& out);

    // Building blocks (public for the tests).
    bool yay0_decompress(const uint8_t* in, size_t in_size, std::vector<uint8_t>& out);
    bool pers_szp_decode(const std::vector<uint8_t>& blob, std::vector<uint8_t>& out);
    Image decode_rgba16(const std::vector<uint8_t>& d, size_t off, int w, int h);
    Image decode_i4_alpha(const std::vector<uint8_t>& d, size_t off, int w, int h);
    Image decode_i4(const std::vector<uint8_t>& d, size_t off, int w, int h);
    Image decode_ia8(const std::vector<uint8_t>& d, size_t off, int w, int h);
    int font_tile_for(char c);             // glyph tile of an ASCII character (0 = blank)
}
