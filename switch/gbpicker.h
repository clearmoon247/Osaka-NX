// Game Boy cartridge picker for the Transfer Pak / GB Tower: finds Pokemon Red/Blue/Yellow ROMs and Gen-1 saves on
// the SD card and lets the player assign them to the four controller ports. Everything here is portable C++17 (no libnx):
// the Linux test in tests/gbpicker_test.cpp drives the same code with synthetic cartridges. The Switch screen
// (gbpicker_switch.cpp) and the test front end implement `Frontend`.
//
// Pokemon Stadium (1) reads Gen-1 carts only: Red, Blue, Yellow (and Japanese Green). Gold/Silver/Crystal belong to Stadium 2.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace gbpicker {
    namespace fs = std::filesystem;

    enum class Kind { Unknown, Red, Blue, Yellow, Green };

    struct Rom {
        fs::path path;
        Kind kind = Kind::Unknown;
        std::string label;   // "Pokemon Yellow"
        std::string note;    // "header checksum mismatch", "Japanese", ... (empty when nothing to say)
    };

    struct Save {
        fs::path path;
        bool valid = false;  // 32 KiB with a correct Gen-1 main-data checksum
        std::string trainer; // decoded only when valid
        std::string id;
        std::string note;    // why it is not valid
    };

    struct Catalog {
        std::vector<Rom> roms;    // supported games only
        std::vector<Save> saves;
        size_t files_seen = 0;
        size_t other_gb_files = 0; // .gb/.gbc files that are not Gen-1 Pokemon
        bool truncated = false;    // hit the entry limit
    };

    struct Root {
        fs::path path;
        int max_depth = 3;
    };

    struct Skip {
        std::vector<std::string> dir_names;  // directory names never entered (lower case), e.g. "nintendo"
    };

    // The folders the SD card scan never enters: the console's and Atmosphere's own data, themes, and app caches that hold many
    // thousands of files. Everything else is searched, including /switch (where homebrew keeps its data) and the other folders
    // on the card.
    Skip default_skip();

    // Looks for .gb/.gbc ROMs and .sav/.srm saves under each root. Reads only the first 0x150 bytes of a ROM, so a big
    // SD card with thousands of files stays quick; `max_entries` bounds the walk.
    Catalog scan(const std::vector<Root>& roots, const Skip& skip, size_t max_entries = 30000);

    // Identifies one ROM file (used by scan, and for paths from launcher.cfg that the scan did not visit).
    bool inspect_rom(const fs::path& path, Rom& out);
    bool inspect_save(const fs::path& path, Save& out);

    // Saves worth offering for `rom`, best first: same file name, same folder, any other valid save, then the rest.
    std::vector<size_t> save_candidates(const Catalog& catalog, const Rom& rom);

    struct Slot {
        fs::path rom;
        fs::path save;
        bool empty() const { return rom.empty(); }
    };
    using Slots = std::array<Slot, 4>;

    // launcher.cfg: the same pN_rom / pN_save keys that src/main/transfer_pak.cpp reads. Relative paths resolve against
    // the config's own folder. write_slots keeps every other line of an existing file.
    Slots read_slots(const fs::path& cfg);
    bool write_slots(const fs::path& cfg, const Slots& slots, const fs::path& base);

    // ---- the screen flow ---------------------------------------------------------------------------------------------
    enum class Key { Up, Down, Left, Right, A, B, X, Y, Plus, Minus };

    // What a screen shows, for front ends that draw more than text lines.
    struct PortView {
        bool used = false;             // a ROM is assigned
        Kind kind = Kind::Unknown;
        std::string game;              // "Red", "Yellow", ... (empty when the ROM could not be read)
        std::string name;              // trainer name, or "No save" / "Bad save" / "Save missing"
        std::string id;                // "ID 20152" (empty without a valid save)
        std::string rom_file, save_file;
        bool problem = false;          // something to look at: missing file, invalid save
    };

    struct View {
        enum class Type { Main, PickRom, PickSave, Message } type = Type::Main;
        std::array<PortView, 4> ports;
        int port = 0;                  // cursor on the main screen, port being edited on the list screens
        std::vector<std::string> items;    // list screens: first column
        std::vector<std::string> details;  // list screens: second column (may be empty strings)
        int cursor = 0;
        std::string subtitle;          // list screens: what the list is for
        std::vector<std::string> notices;  // main screen: warnings
        std::string message;           // message screen
        std::vector<std::pair<std::string, std::string>> hints;  // {"A", "Choose"}
        int found_roms = 0, found_saves = 0;
    };

    class Frontend {
    public:
        virtual ~Frontend() = default;
        virtual void draw(const std::vector<std::string>& lines) = 0;  // replace the whole screen
        // Graphical front ends override this; the default shows the text version.
        virtual void draw_view(const View& view, const std::vector<std::string>& lines) { (void)view; draw(lines); }
        virtual bool wait_key(Key& key) = 0;                           // false = the program is quitting
    };

    enum class Result { Start, Skip, Quit };  // Start: choices saved; Skip: start with launcher.cfg unchanged; Quit: the program is closing

    // Runs until the player starts the game (slots are final and, if changed, written to `cfg`), skips, or quits.
    Result run(Frontend& ui, const Catalog& catalog, Slots& slots, const fs::path& cfg, const fs::path& base);

    // For logging and tests.
    std::string describe(const Catalog& catalog, const Slot& slot, const fs::path& base);
    PortView port_view(const Catalog& catalog, const Slot& slot, const fs::path& base);
    std::string display_path(const fs::path& path, const fs::path& base);
    // Path text as written to launcher.cfg (device prefix removed from rooted paths), the device prefix of a path ("sdmc:"
    // or ""), and a pass over an existing launcher.cfg that rewrites "<device>:/x" values to "/x" (keeps a .bak).
    std::string config_path(const fs::path& path, const fs::path& base);
    std::string device_prefix(const std::string& path);
    bool normalize_config(const fs::path& cfg, const std::string& device);
}
