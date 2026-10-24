// Host test for data_migration.h (the one-time move of sdmc:/switch/pokestadiumrecomp to sdmc:/switch/osaka-nx).
// It builds a fake SD card in a temporary folder and checks each case described at the top of that header:
//   A. nothing to migrate            B. both folders exist and share a file (nothing may be overwritten or deleted)
//   C. the real migration            D. running it a second time (must do nothing)
//   E. an existing osaka-nx.cfg must not be overwritten by an old psrnx.cfg
//   F. the usual real-world case: osaka-nx.nro was installed first, so the new folder exists and holds only that file;
//      the old saves, ROM and launcher.cfg must still arrive, the old .nro must not, and saves present in both survive
// Build and run:  g++ -std=c++17 -o migrate_test migrate_test.cpp && ./migrate_test      (tests/run.sh does this)
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../data_migration.h"

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++g_failures; } } while (0)

static void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

static std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

int main() {
    // A fresh temporary "SD card" for every case, removed at the end.
    const fs::path root = fs::temp_directory_path() / ("migrate_test_" + std::to_string(std::rand()));
    auto fresh = [&]() { fs::remove_all(root); fs::create_directories(root / "switch"); };
    const std::string old_dir = (root / "switch/pokestadiumrecomp").string(), new_dir = (root / "switch/osaka-nx").string();
    auto run = [&]() { return psrnx_migrate::migrate_data_folder(old_dir, new_dir, "/switch/pokestadiumrecomp/", "/switch/osaka-nx/"); };

    // A. Nothing to migrate: empty result and nothing created.
    fresh();
    CHECK(run().empty());
    CHECK(!fs::exists(new_dir));

    // B. Both folders exist and a file is in both: the new file wins, the old one stays untouched, the rest moves.
    fresh();
    write(old_dir + "/ROM.z64", "old");
    write(old_dir + "/other.txt", "only-old");
    write(new_dir + "/ROM.z64", "new");
    const std::string note_b = run();
    CHECK(note_b.find("kept 1") != std::string::npos);
    CHECK(read(old_dir + "/ROM.z64") == "old");
    CHECK(read(new_dir + "/ROM.z64") == "new");
    CHECK(read(new_dir + "/other.txt") == "only-old");
    CHECK(!fs::exists(old_dir + "/other.txt"));

    // C. The real migration: files move with the folder, psrnx.cfg is renamed, absolute paths in launcher.cfg follow.
    fresh();
    write(old_dir + "/ROM.z64", "rom");
    write(old_dir + "/roms/Red.gb", "gb");
    write(old_dir + "/psrnx.cfg", "resolution_factor=3\n");
    write(old_dir + "/launcher.cfg", "port1=" + old_dir.substr(0, old_dir.find("/switch/")) + "/switch/pokestadiumrecomp/roms/Red.gb\nport2=roms/Blue.gb\n");
    const std::string note_c = run();
    CHECK(note_c.find("moved the data folder") != std::string::npos);
    CHECK(!fs::exists(old_dir));
    CHECK(read(new_dir + "/ROM.z64") == "rom");
    CHECK(read(new_dir + "/roms/Red.gb") == "gb");
    CHECK(!fs::exists(new_dir + "/psrnx.cfg"));
    CHECK(read(new_dir + "/osaka-nx.cfg") == "resolution_factor=3\n");
    const std::string launcher = read(new_dir + "/launcher.cfg");
    CHECK(launcher.find("/switch/osaka-nx/roms/Red.gb") != std::string::npos);
    CHECK(launcher.find("pokestadiumrecomp") == std::string::npos);
    CHECK(launcher.find("port2=roms/Blue.gb\n") != std::string::npos);   // relative paths are untouched

    // D. A second run changes nothing and reports nothing.
    CHECK(run().empty());
    CHECK(read(new_dir + "/ROM.z64") == "rom");

    // E. An osaka-nx.cfg that already exists is never replaced by an old psrnx.cfg.
    fresh();
    write(old_dir + "/psrnx.cfg", "old\n");
    write(old_dir + "/osaka-nx.cfg", "keep\n");
    run();
    CHECK(read(new_dir + "/osaka-nx.cfg") == "keep\n");
    CHECK(read(new_dir + "/psrnx.cfg") == "old\n");

    // F. Installed first, started later: the new folder exists with only the program file in it.
    fresh();
    write(new_dir + "/osaka-nx.nro", "new program");
    write(old_dir + "/pokestadiumrecomp.nro", "old program");
    write(old_dir + "/ROM.z64", "rom");
    write(old_dir + "/saves/stadium.bin", "SAVE-A");
    write(old_dir + "/saves/other/slot.bin", "SAVE-B");
    write(old_dir + "/launcher.cfg", "port1=/switch/pokestadiumrecomp/roms/Red.gb\n");
    write(new_dir + "/saves/other/extra.bin", "new-extra");      // a folder present in both is merged, not replaced
    const std::string note_f = run();
    CHECK(note_f.find("moved") != std::string::npos);
    CHECK(read(new_dir + "/osaka-nx.nro") == "new program");
    CHECK(!fs::exists(new_dir + "/pokestadiumrecomp.nro"));     // the old program file is not carried over
    CHECK(read(old_dir + "/pokestadiumrecomp.nro") == "old program");
    CHECK(read(new_dir + "/ROM.z64") == "rom");
    CHECK(read(new_dir + "/saves/stadium.bin") == "SAVE-A");
    CHECK(read(new_dir + "/saves/other/slot.bin") == "SAVE-B");
    CHECK(read(new_dir + "/saves/other/extra.bin") == "new-extra");
    CHECK(read(new_dir + "/launcher.cfg").find("/switch/osaka-nx/roms/Red.gb") != std::string::npos);
    CHECK(run().empty());                                       // nothing left to do on the next start
    // F2. A save that exists on both sides is never replaced.
    fresh();
    write(old_dir + "/saves/stadium.bin", "OLD-SAVE");
    write(new_dir + "/saves/stadium.bin", "NEW-SAVE");
    const std::string note_f2 = run();
    CHECK(note_f2.find("kept 1") != std::string::npos);
    CHECK(read(new_dir + "/saves/stadium.bin") == "NEW-SAVE");
    CHECK(read(old_dir + "/saves/stadium.bin") == "OLD-SAVE");

    fs::remove_all(root);
    std::printf(g_failures == 0 ? "MIGRATE OK\n" : "MIGRATE FAILED (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
