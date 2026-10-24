// Renders sample picker screens with assets from a dump made by tools/dump_stadium_ui.py and writes PPM files.
//   gfx_preview <dump dir> <out dir>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "../gbpicker_gfx.h"

static std::vector<uint8_t> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

static void save(const gbpicker::Gfx& g, const std::string& path) {
    std::vector<uint32_t> out(1280 * 720);
    g.present(out.data(), 1280, 720, 1280);
    FILE* f = fopen(path.c_str(), "wb");
    fprintf(f, "P6\n1280 720\n255\n");
    for (uint32_t p : out) { unsigned char c[3] = {(unsigned char)p, (unsigned char)(p >> 8), (unsigned char)(p >> 16)}; fwrite(c, 1, 3, f); }
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string d = argv[1], o = argv[2];
    skin::Sources s;
    s.font = slurp(d + "/fonts/3.bin");
    s.menu2 = slurp(d + "/common_menu2_ui/0.bin");
    s.gbpak = slurp(d + "/gb_pak_select_ui/0.bin");
    s.menusel = slurp(d + "/menu_select_ui/0.bin");
    skin::Skin sk; std::string err;
    if (!skin::build(s, sk, err)) { fprintf(stderr, "skin: %s\n", err.c_str()); return 1; }
    gbpicker::Gfx g(sk);
    using namespace gbpicker;
    View v;
    v.type = View::Type::Main;
    v.ports[0] = {true, Kind::Red, "Red", "RED", "ID 20152", "Pokemon Red.gb", "Pokemon Red.sav", false};
    v.ports[1] = {true, Kind::Blue, "Blue", "ASH", "ID 00123", "Pokemon Blue.gb", "Pokemon Blue.sav", false};
    v.ports[2] = {true, Kind::Yellow, "Yellow", "No save", "", "Pokemon Yellow.gb", "", true};
    v.port = 1;
    v.hints = {{"A", "Cartridge"}, {"X", "Save"}, {"Y", "Clear"}, {"+", "Start"}, {"B", "Skip"}};
    g.render(v); save(g, o + "/main.ppm");
    v.type = View::Type::PickRom; v.subtitle = "Port 2: choose a cartridge";
    v.items = {"Pokemon Red.gb", "Pokemon Blue.gb", "Pokemon Yellow.gb", "Pokemon Green (J).gb", "Hack.gb", "Another long file name for testing.gb", "More.gb"};
    v.details = {"Red", "Blue", "Yellow", "Green", "Unknown", "Blue", "Red"};
    v.cursor = 2; v.hints = {{"A", "Choose"}, {"B", "Back"}};
    g.render(v); save(g, o + "/list.ppm");
    v.type = View::Type::Message; v.hints.clear(); v.message = "Starting the game...  Press L, R or - now to change your Game Paks.";
    g.render(v); save(g, o + "/prompt.ppm");
    v.message = "No Game Boy ROMs were found on the SD card. Put them in sdmc:/roms and try again.";
    g.render(v); save(g, o + "/message.ppm");
    g.splash("Please", "Wait", {"Preparing the graphics.", "The game starts when the bar is full.", "This only happens at start-up."}, 0.42f, "56 / 134");
    save(g, o + "/splash.ppm");
    return 0;
}
