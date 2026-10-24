#include "stadium_skin.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace skin {
namespace {
    uint32_t be32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3]; }
    uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

    // ROM offsets (USA, pret/pokestadium yamls/us/rom.yaml). A bin ends where the next one starts.
    struct Range { uint32_t start, end; };
    constexpr Range kFonts{0x3BA190, 0x3CB130};
    constexpr Range kMenu2{0x4BE810, 0x4D1330};
    constexpr Range kGbPak{0x6B4F00, 0x6C1AB0};
    constexpr Range kMenuSelect{0x4EB0C0, 0x5046B0};
    // Header CRCs of the known USA dump, ROM bytes 0x10..0x17.
    constexpr uint32_t kCrc1 = 0x90F5D9B3, kCrc2 = 0x9D0EDCF0;

    // Byte order of the dump: z64 is big-endian (what the offsets above assume), v64 swaps each byte pair, n64 each 4-byte word.
    enum class Order { Z64, V64, N64 };

    bool detect_order(const uint8_t* h, Order& order) {
        if (h[0] == 0x80 && h[1] == 0x37 && h[2] == 0x12 && h[3] == 0x40) { order = Order::Z64; return true; }
        if (h[0] == 0x37 && h[1] == 0x80 && h[2] == 0x40 && h[3] == 0x12) { order = Order::V64; return true; }
        if (h[0] == 0x40 && h[1] == 0x12 && h[2] == 0x37 && h[3] == 0x80) { order = Order::N64; return true; }
        return false;
    }

    // Reads [start, end) in z64 order. Every range used here starts and ends on a 4-byte boundary.
    bool read_range(std::ifstream& f, Range r, Order order, std::vector<uint8_t>& out) {
        out.resize(r.end - r.start);
        f.seekg(r.start);
        f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!f) return false;
        if (order == Order::V64) for (size_t i = 0; i + 1 < out.size(); i += 2) std::swap(out[i], out[i + 1]);
        if (order == Order::N64) for (size_t i = 0; i + 3 < out.size(); i += 4) { std::swap(out[i], out[i + 3]); std::swap(out[i + 1], out[i + 2]); }
        return true;
    }

    // Multi-asset bin: header words {?, 0, total size, count}, then count x {offset, size, ?, ?}.
    bool multi_file(const std::vector<uint8_t>& bin, uint32_t index, std::vector<uint8_t>& out) {
        if (bin.size() < 0x10 || be32(&bin[4]) != 0) return false;
        const uint32_t count = be32(&bin[12]);
        if (index >= count || bin.size() < 0x10 + static_cast<size_t>(count) * 16) return false;
        const uint32_t off = be32(&bin[0x10 + index * 16]), size = be32(&bin[0x14 + index * 16]);
        if (off > bin.size() || size > bin.size() - off) return false;
        out.assign(bin.begin() + off, bin.begin() + off + size);
        return true;
    }

    uint32_t rgba16_to_px(uint16_t v) {
        const uint32_t r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
        return rgba(static_cast<uint8_t>(r * 255 / 31), static_cast<uint8_t>(g * 255 / 31), static_cast<uint8_t>(b * 255 / 31), (v & 1) ? 255 : 0);
    }
}

bool yay0_decompress(const uint8_t* in, size_t in_size, std::vector<uint8_t>& out) {
    if (in_size < 16 || std::memcmp(in, "Yay0", 4) != 0) return false;
    const uint32_t size = be32(in + 4);
    size_t link = be32(in + 8), chunk = be32(in + 12), mask_pos = 16;
    if (size > (64u << 20) || link > in_size || chunk > in_size) return false;
    out.clear();
    out.reserve(size);
    uint32_t bits = 0;
    int left = 0;
    while (out.size() < size) {
        if (left == 0) {
            if (mask_pos + 4 > in_size) return false;
            bits = be32(in + mask_pos);
            mask_pos += 4;
            left = 32;
        }
        if (bits & 0x80000000u) {
            if (chunk >= in_size) return false;
            out.push_back(in[chunk++]);
        } else {
            if (link + 2 > in_size) return false;
            const uint16_t l = be16(in + link);
            link += 2;
            const size_t dist = (l & 0xFFFu) + 1;
            size_t n = l >> 12;
            if (n == 0) {
                if (chunk >= in_size) return false;
                n = static_cast<size_t>(in[chunk++]) + 18;
            } else {
                n += 2;
            }
            if (dist > out.size()) return false;
            for (size_t i = 0; i < n && out.size() < size; ++i) out.push_back(out[out.size() - dist]);
        }
        bits <<= 1;
        --left;
    }
    return true;
}

// PERS-SZP: magic[8], header_size, size1, size2, patch count, then `count` {value, offset} word patches, then Yay0 at header_size.
bool pers_szp_decode(const std::vector<uint8_t>& blob, std::vector<uint8_t>& out) {
    if (blob.size() < 0x18 || std::memcmp(blob.data(), "PERS-SZP", 8) != 0) return false;
    const uint32_t header_size = be32(&blob[8]), size2 = be32(&blob[16]), patches = be32(&blob[20]);
    if (header_size > blob.size() || size2 > (64u << 20) || patches > (blob.size() - 0x18) / 8) return false;
    if (!yay0_decompress(blob.data() + header_size, blob.size() - header_size, out)) return false;
    if (out.size() < size2) out.resize(size2, 0);
    for (uint32_t i = 0; i < patches; ++i) {
        const uint32_t value = be32(&blob[0x18 + i * 8]), offset = be32(&blob[0x1C + i * 8]);
        if (offset <= out.size() && out.size() - offset >= 4) {
            out[offset] = static_cast<uint8_t>(value >> 24); out[offset + 1] = static_cast<uint8_t>(value >> 16);
            out[offset + 2] = static_cast<uint8_t>(value >> 8); out[offset + 3] = static_cast<uint8_t>(value);
        }
    }
    return true;
}

Image decode_rgba16(const std::vector<uint8_t>& d, size_t off, int w, int h) {
    Image im;
    if (off + static_cast<size_t>(w) * h * 2 > d.size()) return im;
    im.w = w; im.h = h; im.px.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < im.px.size(); ++i) im.px[i] = rgba16_to_px(be16(&d[off + i * 2]));
    return im;
}

Image decode_i4_alpha(const std::vector<uint8_t>& d, size_t off, int w, int h) {
    Image im;
    if (off + static_cast<size_t>(w) * h / 2 > d.size()) return im;
    im.w = w; im.h = h; im.px.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < im.px.size(); ++i) {
        const uint8_t b = d[off + i / 2];
        const uint8_t v = static_cast<uint8_t>(((i & 1) ? (b & 15) : (b >> 4)) * 17);
        im.px[i] = rgba(255, 255, 255, v);   // the game tints it with the environment colour; intensity is the coverage
    }
    return im;
}

Image decode_i4(const std::vector<uint8_t>& d, size_t off, int w, int h) {
    Image im;
    if (off + static_cast<size_t>(w) * h / 2 > d.size()) return im;
    im.w = w; im.h = h; im.px.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < im.px.size(); ++i) {
        const uint8_t b = d[off + i / 2];
        const uint8_t v = static_cast<uint8_t>(((i & 1) ? (b & 15) : (b >> 4)) * 17);
        im.px[i] = rgba(v, v, v, 255);
    }
    return im;
}

Image decode_ia8(const std::vector<uint8_t>& d, size_t off, int w, int h) {
    Image im;
    if (off + static_cast<size_t>(w) * h > d.size()) return im;
    im.w = w; im.h = h; im.px.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < im.px.size(); ++i) {
        const uint8_t v = d[off + i];
        const uint8_t c = static_cast<uint8_t>((v >> 4) * 17), a = static_cast<uint8_t>((v & 15) * 17);
        im.px[i] = rgba(c, c, c, a);
    }
    return im;
}

int font_tile_for(char c) {
    if (c >= '0' && c <= '9') return 0x10 + (c - '0');
    if (c >= 'A' && c <= 'F') return 0x1A + (c - 'A');
    if (c >= 'G' && c <= 'V') return 0x20 + (c - 'G');
    if (c >= 'W' && c <= 'Z') return 0x30 + (c - 'W');
    if (c >= 'a' && c <= 'z') return 0x45 + (c - 'a');
    switch (c) {
    case ' ': return 0x00;
    case '(': return 0x03; case ')': return 0x04; case '%': return 0x06; case '[': return 0x07; case ']': return 0x08;
    case '&': return 0x0D; case '~': return 0x0E;
    case '!': return 0x34; case '"': return 0x35; case '\'': return 0x37; case '*': return 0x38; case '+': return 0x39;
    case ',': return 0x3A; case '-': case '_': return 0x3B; case '.': return 0x3C; case '/': return 0x3D; case ':': return 0x3E;
    case '=': return 0x3F; case '?': return 0x40; case ';': return 0x42;
    default: return 0x40;   // '?'
    }
}

bool build(const Sources& src, Skin& out, std::string& error) {
    out = Skin{};
    if (src.font.size() < 0x90 + static_cast<size_t>(Font::kTiles) * Font::kCellW * Font::kCellH) { error = "font asset too small"; return false; }
    std::memcpy(out.font.widths, src.font.data(), sizeof(out.font.widths));
    out.font.tiles.assign(src.font.begin() + 0x90, src.font.end());

    // Segment 2 (common_menu2_ui): corners (I4 16x16), cartridge icons (RGBA16 64x64), "1P".."4P" (IA8 32x24).
    const uint32_t corner_off[4] = {0x288E0, 0x28960, 0x287E0, 0x28860};     // TL, TR, BL, BR as the game's panel code uses them
    for (int i = 0; i < 4; ++i) out.corner[i] = decode_i4_alpha(src.menu2, corner_off[i], 16, 16);
    // Game cart type -> texture: 1 red 0x1E420, 2 green 0x20420, 3 blue 0x22420, 7 yellow 0x24420.
    const uint32_t cart_tex[4] = {0x1E420, 0x20420, 0x22420, 0x24420};
    for (int i = 0; i < 4; ++i) out.cart[i] = decode_rgba16(src.menu2, cart_tex[i], 64, 64);
    const uint32_t label_off[4] = {0x1D820, 0x1DB20, 0x1DE20, 0x1E120};
    for (int i = 0; i < 4; ++i) out.label[i] = decode_ia8(src.menu2, label_off[i], 32, 24);
    out.arrow = decode_rgba16(src.gbpak, 0x6060, 48, 16);   // segment 3
    out.logo_tile = decode_i4(src.menusel, 0xF780, 128, 64);  // menu_select_ui: the repeating logo of the background

    for (const Image* im : {&out.corner[0], &out.corner[1], &out.corner[2], &out.corner[3], &out.cart[0], &out.cart[1], &out.cart[2],
                            &out.cart[3], &out.label[0], &out.label[1], &out.label[2], &out.label[3], &out.arrow, &out.logo_tile}) {
        if (im->empty()) { error = "an asset is smaller than expected (different ROM revision?)"; return false; }
    }

    out.ok = true;
    return true;
}

bool read_rom_sources(const std::filesystem::path& rom, Sources& out, std::string& error) {
    std::ifstream f(rom, std::ios::binary);
    if (!f) { error = "ROM not found"; return false; }
    uint8_t head[0x18];
    f.read(reinterpret_cast<char*>(head), sizeof(head));
    Order order = Order::Z64;
    if (!f || !detect_order(head, order)) { error = "not an N64 ROM"; return false; }
    {   // header CRCs, in z64 order
        std::vector<uint8_t> hdr;
        if (!read_range(f, Range{0, 0x20}, order, hdr) || be32(&hdr[0x10]) != kCrc1 || be32(&hdr[0x14]) != kCrc2) {
            error = "not the known USA ROM (header CRC differs)";
            return false;
        }
    }

    std::vector<uint8_t> bin, file;
    if (!read_range(f, kFonts, order, bin) || !multi_file(bin, 3, file) || !pers_szp_decode(file, out.font)) { error = "font asset unreadable"; return false; }
    if (!read_range(f, kMenu2, order, bin) || !pers_szp_decode(bin, out.menu2)) { error = "common_menu2_ui unreadable"; return false; }
    if (!read_range(f, kGbPak, order, bin) || !pers_szp_decode(bin, out.gbpak)) { error = "gb_pak_select_ui unreadable"; return false; }
    if (!read_range(f, kMenuSelect, order, bin) || !pers_szp_decode(bin, out.menusel)) { error = "menu_select_ui unreadable"; return false; }
    return true;
}

bool load_from_folder(const std::filesystem::path& dir, Skin& out) {
    std::error_code ec;
    std::string last = "no ROM file in the folder";
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::string ext = entry.path().extension().string();
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ext != ".z64" && ext != ".n64" && ext != ".v64") continue;
        if (load_from_rom(entry.path(), out)) return true;
        last = out.error;
    }
    out = Skin{};
    out.error = last;
    return false;
}

bool load_from_rom(const std::filesystem::path& rom, Skin& out) {
    Sources src;
    std::string error;
    if (!read_rom_sources(rom, src, error) || !build(src, out, error)) {
        out = Skin{};
        out.error = error;
        return false;
    }
    return true;
}
}
