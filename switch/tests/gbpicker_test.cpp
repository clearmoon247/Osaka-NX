// Linux test for the cartridge picker: builds synthetic Pokemon carts and saves in a temp folder, then checks scanning,
// save matching, launcher.cfg round-trips and the full screen flow with a scripted front end.
//   switch/tests/run.sh        (set GBPICKER_SHOW=1 to print every screen)
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include "../gbpicker.h"

using namespace gbpicker;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

static void write_file(const fs::path& p, const std::vector<uint8_t>& data) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

static void make_rom(const fs::path& p, const char* title, bool good_checksum = true, bool japanese = false, size_t size = 0x100000) {
    std::vector<uint8_t> rom(size, 0);
    std::memcpy(&rom[0x134], title, std::strlen(title));
    rom[0x148] = 5;
    rom[0x14A] = japanese ? 0 : 1;
    uint8_t c = 0;
    for (size_t i = 0x134; i <= 0x14C; ++i) c = static_cast<uint8_t>(c - rom[i] - 1);
    rom[0x14D] = good_checksum ? c : static_cast<uint8_t>(c + 1);
    write_file(p, rom);
}

static uint8_t enc(char ch) {
    if (ch >= 'A' && ch <= 'Z') return static_cast<uint8_t>(0x80 + ch - 'A');
    if (ch >= 'a' && ch <= 'z') return static_cast<uint8_t>(0xA0 + ch - 'a');
    return 0x7F;
}

static void make_save(const fs::path& p, const char* name, int id, bool valid = true, size_t size = 0x8000) {
    std::vector<uint8_t> sav(size, 0);
    if (valid) {
        size_t i = 0;
        for (; name[i]; ++i) sav[0x2598 + i] = enc(name[i]);
        sav[0x2598 + i] = 0x50;
        sav[0x2605] = static_cast<uint8_t>(id >> 8);
        sav[0x2606] = static_cast<uint8_t>(id);
        uint32_t sum = 0;
        for (size_t k = 0x2598; k <= 0x3522; ++k) sum += sav[k];
        sav[0x3523] = static_cast<uint8_t>(~(sum & 0xFF));
    }
    write_file(p, sav);
}

class Scripted : public Frontend {
public:
    std::vector<Key> keys;
    size_t next = 0;
    std::vector<std::string> last;
    void draw(const std::vector<std::string>& lines) override {
        last = lines;
        if (std::getenv("GBPICKER_SHOW")) {
            std::cout << "----------------------------------------------------------------------------\n";
            for (const auto& l : lines) std::cout << l << "\n";
        }
    }
    bool wait_key(Key& key) override {
        if (next >= keys.size()) return false;
        key = keys[next++];
        if (std::getenv("GBPICKER_SHOW")) std::cout << "  <key " << static_cast<int>(key) << ">\n";
        return true;
    }
    bool screen_has(const char* text) const {
        for (const auto& l : last) if (l.find(text) != std::string::npos) return true;
        return false;
    }
};

static std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int main() {
    const fs::path base = fs::temp_directory_path() / "gbpicker_test_data";
    fs::remove_all(base);
    fs::create_directories(base);

    make_rom(base / "gb/pokemon-red.gb", "POKEMON RED");
    make_save(base / "gb/pokemon-red.sav", "ASH", 12345);
    make_rom(base / "gb/pokemon-yellow.gbc", "POKEMON YELLOW");
    make_save(base / "gb/pokemon-yellow.sav", "GARY", 4242);
    make_rom(base / "gb/Pokemon Blue.gb", "POKEMON BLUE", false);
    make_save(base / "gb/Pokemon Blue.gb.sav", "MISTY", 7);
    make_rom(base / "gb/tetris.gb", "TETRIS");
    make_rom(base / "gb/gold.gbc", "POKEMON_GLDAAUE");   // Gen 2: not offered
    make_rom(base / "gb/green-jp.gb", "POKEMON GREEN", true, true);
    make_rom(base / "gb/short.gb", "POKEMON RED", true, false, 0x80000);
    make_save(base / "saves/loose.sav", "BROCK", 99);
    make_save(base / "saves/empty.sav", "", 0, false);
    make_save(base / "saves/gen2.srm", "", 0, false, 0x8000);
    make_save(base / "saves/tiny.sav", "", 0, false, 100);
    make_rom(base / "a/b/c/d/e/deep.gb", "POKEMON RED");
    make_rom(base / "Nintendo/hidden.gb", "POKEMON RED");
    make_rom(base / "yellow-hack/yellow-hack.gbc", "POKEMON YELLOW");
    make_save(base / "yellow-hack/one.sav", "A", 1);
    make_save(base / "yellow-hack/two.sav", "B", 2);

    // ---- scan -------------------------------------------------------------------------------------------------------
    const Catalog catalog = scan({{base, 3}}, Skip{{"nintendo"}});
    std::printf("scan: %zu roms, %zu saves, %zu other gb files, %zu entries\n", catalog.roms.size(), catalog.saves.size(),
                catalog.other_gb_files, catalog.files_seen);
    CHECK(catalog.roms.size() == 6);          // red, yellow, blue, green-jp, short, yellow-hack (deep + Nintendo skipped)
    CHECK(catalog.other_gb_files == 2);       // tetris, gold
    CHECK(catalog.saves.size() == 9);
    int valid = 0;
    for (const Save& s : catalog.saves) valid += s.valid;
    CHECK(valid == 6);
    for (const Rom& r : catalog.roms) {
        if (r.path.filename() == "Pokemon Blue.gb") CHECK(r.note.find("header checksum") != std::string::npos);
        if (r.path.filename() == "green-jp.gb") CHECK(r.kind == Kind::Green && r.note.find("Japanese") != std::string::npos);
        if (r.path.filename() == "short.gb") CHECK(r.note.find("file size") != std::string::npos);
    }
    CHECK(scan({{base, 3}}, Skip{{}}, 5).truncated);   // entry limit holds

    // The default skip list: /switch and other app folders are searched, system data and app caches are not.
    {
        const fs::path card = base / "fake_sd";
        make_rom(card / "switch/someapp/gb/in_switch.gb", "POKEMON RED");
        make_rom(card / "switch/osaka-nx/gb/data_folder.gb", "POKEMON BLUE");
        make_rom(card / "games/gb/elsewhere.gb", "POKEMON YELLOW");
        make_save(card / "switch/someapp/gb/in_switch.sav", "A", 1);
        make_rom(card / "atmosphere/contents/sys.gb", "POKEMON RED");
        make_rom(card / "switch/.overlays/ovl.gb", "POKEMON RED");
        make_rom(card / "switch/appstore/cache.gb", "POKEMON RED");
        make_rom(card / "emummc/e.gb", "POKEMON RED");
        const Catalog sd = scan({{card, 3}}, default_skip());
        CHECK(sd.roms.size() == 3);
        CHECK(sd.saves.size() == 1);
        for (const Rom& r : sd.roms) CHECK(r.path.string().find("atmosphere") == std::string::npos && r.path.string().find(".overlays") == std::string::npos &&
                                           r.path.string().find("appstore") == std::string::npos && r.path.string().find("emummc") == std::string::npos);
    }

    auto rom_named = [&](const char* name) -> const Rom& {
        for (const Rom& r : catalog.roms) if (r.path.filename() == name) return r;
        std::fprintf(stderr, "rom %s not found\n", name);
        std::exit(1);
    };
    // save matching: same name first, other valid saves next, junk last
    auto cands = save_candidates(catalog, rom_named("pokemon-red.gb"));
    CHECK(catalog.saves[cands[0]].path.filename() == "pokemon-red.sav");
    CHECK(!catalog.saves[cands.back()].valid);
    cands = save_candidates(catalog, rom_named("Pokemon Blue.gb"));
    CHECK(catalog.saves[cands[0]].path.filename() == "Pokemon Blue.gb.sav");   // "<rom file name>.sav" convention
    cands = save_candidates(catalog, rom_named("yellow-hack.gbc"));
    CHECK(catalog.saves[cands[0]].valid && catalog.saves[cands[1]].valid);     // same-folder saves before the others
    CHECK(catalog.saves[cands[0]].path.parent_path().filename() == "yellow-hack");
    std::printf("scan and matching: ok\n");

    // ---- launcher.cfg round trip -------------------------------------------------------------------------------------
    const fs::path cfg = base / "launcher.cfg";
    {
        std::ofstream f(cfg);
        f << "# my settings\nautoplay=on\np1_rom=gb/pokemon-red.gb\np1_save=gb/pokemon-red.sav\np3_rom_off=gb/x.gb\n";
    }
    Slots slots = read_slots(cfg);
    CHECK(slots[0].rom == base / "gb/pokemon-red.gb" && slots[0].save == base / "gb/pokemon-red.sav");
    CHECK(slots[1].empty() && slots[2].empty());
    slots[1] = {base / "gb/pokemon-yellow.gbc", base / "gb/pokemon-yellow.sav"};
    slots[0] = {};
    CHECK(write_slots(cfg, slots, base));
    const std::string text = slurp(cfg);
    CHECK(text.find("autoplay=on") != std::string::npos && text.find("p3_rom_off=gb/x.gb") != std::string::npos);
    CHECK(text.find("p1_rom") == std::string::npos);
    CHECK(text.find("p2_rom=gb/pokemon-yellow.gbc") != std::string::npos && text.find("p2_save=gb/pokemon-yellow.sav") != std::string::npos);
    CHECK(!fs::exists(fs::path(cfg).concat(".tmp")));
    std::printf("launcher.cfg round trip: ok\n");

    // ---- screen flow -------------------------------------------------------------------------------------------------
    // roms sorted by path: Pokemon Blue.gb, green-jp.gb, pokemon-red.gb, pokemon-yellow.gbc, short.gb, yellow-hack.gbc
    fs::remove(cfg);
    {
        Scripted ui;
        // port 1: red (own save is taken automatically); port 2: blue; start
        ui.keys = {Key::A, Key::Down, Key::Down, Key::A,
                   Key::Down, Key::A, Key::A,
                   Key::Plus};
        Slots s;
        CHECK(run(ui, catalog, s, cfg, base) == Result::Start);
        CHECK(s[0].rom.filename() == "pokemon-red.gb" && s[0].save.filename() == "pokemon-red.sav");
        CHECK(s[1].rom.filename() == "Pokemon Blue.gb" && s[1].save.filename() == "Pokemon Blue.gb.sav");
        const Slots back = read_slots(cfg);
        CHECK(back[0].rom == s[0].rom && back[1].save == s[1].save && back[2].empty());
        std::printf("flow 1 (auto-picked saves): ok\n");
    }
    {
        Scripted ui;
        // a ROM without its own save: the save list opens; pick the second entry, then clear port 1 and skip with B
        ui.keys = {Key::A, Key::Down, Key::Down, Key::Down, Key::Down, Key::Down, Key::A,   // yellow-hack
                   Key::Down, Key::A};                                                         // second save in the list
        Slots s;
        fs::remove(cfg);
        CHECK(run(ui, catalog, s, cfg, base) == Result::Quit);   // script ends on the main screen: the program is closing
        CHECK(s[0].rom.filename() == "yellow-hack.gbc" && s[0].save.filename() == "two.sav");
        CHECK(ui.screen_has("yellow-hack/two.sav"));
        CHECK(!fs::exists(cfg));                                  // nothing is written until the player confirms
        std::printf("flow 2 (save list): ok\n");
    }
    {
        Scripted ui;
        ui.keys = {Key::A, Key::Down, Key::A, Key::B, Key::Y, Key::B};
        Slots s;
        CHECK(run(ui, catalog, s, cfg, base) == Result::Skip);
        CHECK(s[0].empty());                                      // chosen then cleared
        CHECK(!fs::exists(cfg));
        std::printf("flow 3 (clear, skip): ok\n");
    }
    {
        Scripted ui;
        ui.keys = {Key::A, Key::A};   // no ROMs at all: a message, then back
        Catalog none;
        Slots s;
        CHECK(run(ui, none, s, cfg, base) == Result::Quit);
        std::printf("flow 4 (no roms): ok\n");
    }
    {
        Scripted ui;
        ui.keys = {Key::X, Key::Plus};   // "change save only" with an empty port explains itself
        Slots s;
        CHECK(run(ui, catalog, s, cfg, base) == Result::Start);
        CHECK(!fs::exists(cfg));
        std::printf("flow 5 (save-only on empty port): ok\n");
    }
    {   // Left/Right move the cursor like Up/Down: Right = next port, Left = previous, clamped at the ends.
        Scripted ui;
        ui.keys = {Key::Right, Key::Right, Key::Left, Key::Plus};
        Slots s;
        CHECK(run(ui, catalog, s, cfg, base) == Result::Start);
        CHECK(ui.screen_has("> Port 2"));
        Scripted ui2;
        ui2.keys = {Key::Down, Key::Right, Key::Right, Key::Right, Key::Right, Key::Up, Key::Left, Key::Plus};
        Slots s2;
        CHECK(run(ui2, catalog, s2, cfg, base) == Result::Start);
        CHECK(ui2.screen_has("> Port 2"));                 // 1 -> 4 (clamped) -> 3 -> 2
        std::printf("flow 6 (left/right navigation): ok\n");
    }
    {   // Device paths: std::filesystem calls "sdmc:/x" relative, so the config must hold rooted paths ("/x").
        CHECK(device_prefix("sdmc:/switch/psr") == "sdmc:");
        CHECK(device_prefix("/tmp/a:b/c").empty() && device_prefix("a/b").empty() && device_prefix("C:foo").empty());
        const fs::path dev_base("sdmc:/switch/osaka-nx");
        CHECK(config_path(fs::path("sdmc:/All Roms/Red.gb"), dev_base) == "/All Roms/Red.gb");
        CHECK(config_path(fs::path("sdmc:/switch/osaka-nx/roms/Red.gb"), dev_base) == "roms/Red.gb");
        CHECK(config_path(fs::path("/tmp/x/Red.gb"), fs::path("/tmp/y")) == "/tmp/x/Red.gb");   // no device: unchanged
        const fs::path norm = base / "norm.cfg";
        {
            std::ofstream out(norm);
            out << "# keep\np1_rom=sdmc:/All Roms/Red.gb\np1_save = sdmc:/All Roms/Red.sav ; note\np2_rom=roms/Blue.gb\n";
        }
        CHECK(normalize_config(norm, "sdmc:"));
        CHECK(!normalize_config(norm, "sdmc:"));   // idempotent
        CHECK(fs::exists(fs::path(norm).concat(".bak")));
        std::ifstream in(norm);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(all.find("p1_rom=/All Roms/Red.gb\n") != std::string::npos);
        CHECK(all.find("p1_save =/All Roms/Red.sav") != std::string::npos || all.find("p1_save =") != std::string::npos);
        CHECK(all.find("sdmc:") == std::string::npos);
        CHECK(all.find("p2_rom=roms/Blue.gb\n") != std::string::npos && all.find("# keep") != std::string::npos);
        std::printf("device paths: ok\n");
    }
    fs::remove_all(base);
    std::printf("ALL OK\n");
    return 0;
}
