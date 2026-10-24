// Checks stadium_skin.cpp end to end: a synthetic ROM (tests/make_synth_rom.py) in z64/v64/n64 byte order must load to the
// same skin as the dump's decoded files. Usage: skin_test <dump dir> <synthetic rom dir>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "../stadium_skin.h"

static std::vector<uint8_t> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static bool same(const skin::Image& a, const skin::Image& b) { return a.w == b.w && a.h == b.h && a.px == b.px && !a.empty(); }

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: skin_test <dump> <rom dir>\n"); return 2; }
    const std::string d = argv[1], r = argv[2];
    skin::Sources s;
    s.font = slurp(d + "/fonts/3.bin");
    s.menu2 = slurp(d + "/common_menu2_ui/0.bin");
    s.gbpak = slurp(d + "/gb_pak_select_ui/0.bin");
    s.menusel = slurp(d + "/menu_select_ui/0.bin");
    skin::Skin want; std::string err;
    CHECK(skin::build(s, want, err));
    for (const char* name : {"synth.z64", "synth.v64", "synth.n64"}) {
        skin::Skin got;
        const bool ok = skin::load_from_rom(r + "/" + name, got);
        std::printf("%s: %s %s\n", name, ok ? "loaded" : "FAILED", got.error.c_str());
        CHECK(ok);
        if (!ok) continue;
        CHECK(got.font.tiles.size() >= want.font.tiles.size() && std::equal(want.font.tiles.begin(), want.font.tiles.end(), got.font.tiles.begin()));   // the synthetic file asks for 16 extra zero bytes
        CHECK(std::equal(got.font.widths, got.font.widths + 0x90, want.font.widths));
        CHECK(same(got.logo_tile, want.logo_tile));
        CHECK(same(got.arrow, want.arrow));
        for (int i = 0; i < 4; ++i) { CHECK(same(got.cart[i], want.cart[i])); CHECK(same(got.corner[i], want.corner[i])); CHECK(same(got.label[i], want.label[i])); }
    }
    skin::Skin bad;
    CHECK(!skin::load_from_rom(r + "/bad.z64", bad));
    CHECK(!skin::load_from_rom(r + "/missing.z64", bad));
    // folder search finds the ROM among other files
    skin::Skin folder;
    CHECK(skin::load_from_folder(r, folder));
    std::printf(failures == 0 ? "SKIN OK\n" : "SKIN FAILURES: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
