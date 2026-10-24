// See gbpicker.h.
#include "gbpicker.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <system_error>

namespace gbpicker {
namespace {
    std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string trim(std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
            s = s.substr(1, s.size() - 2);
        }
        return s;
    }

    bool read_head(const fs::path& path, std::vector<uint8_t>& out, size_t bytes, uint64_t* file_size = nullptr) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec) return false;
        if (file_size) *file_size = size;
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        out.assign(std::min<uint64_t>(bytes, size), 0);
        file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        out.resize(static_cast<size_t>(file.gcount()));
        return true;
    }

    // Gen-1 text encoding (the subset a trainer name can use).
    char gen1_char(uint8_t b) {
        if (b >= 0x80 && b <= 0x99) return static_cast<char>('A' + (b - 0x80));
        if (b >= 0xA0 && b <= 0xB9) return static_cast<char>('a' + (b - 0xA0));
        if (b >= 0xF6) return static_cast<char>('0' + (b - 0xF6));
        if (b == 0x7F) return ' ';
        return '\0';
    }

    constexpr uint64_t kSaveSize = 0x8000;
    constexpr size_t kChecksumFirst = 0x2598, kChecksumLast = 0x3522, kChecksumAt = 0x3523;

    std::string ext_lower(const fs::path& p) { return lower(p.extension().string()); }

    int score_save(const fs::path& rom, const Save& save) {
        const std::string rom_stem = lower(rom.stem().string());
        const std::string rom_name = lower(rom.filename().string());
        const std::string save_stem = lower(save.path.stem().string());
        const bool same_dir = save.path.parent_path() == rom.parent_path();
        const bool same_name = save_stem == rom_stem || save_stem == rom_name;
        if (same_dir && same_name) return save.valid ? 0 : 3;
        if (same_name) return save.valid ? 1 : 3;
        if (same_dir && save.valid) return 2;
        return save.valid ? 4 : 5;
    }
}

bool inspect_rom(const fs::path& path, Rom& out) {
    std::vector<uint8_t> head;
    uint64_t size = 0;
    if (!read_head(path, head, 0x150, &size) || head.size() < 0x150) return false;
    out = Rom{};
    out.path = path;

    std::string title(reinterpret_cast<const char*>(head.data() + 0x134), 16);
    for (char& c : title) if (c == '\0') c = ' ';
    title = lower(title);
    if (title.find("pokemon") == std::string::npos) return true;  // some other Game Boy game: Kind::Unknown
    if (title.find("yellow") != std::string::npos) { out.kind = Kind::Yellow; out.label = "Pokemon Yellow"; }
    else if (title.find("red") != std::string::npos) { out.kind = Kind::Red; out.label = "Pokemon Red"; }
    else if (title.find("blue") != std::string::npos) { out.kind = Kind::Blue; out.label = "Pokemon Blue"; }
    else if (title.find("green") != std::string::npos) { out.kind = Kind::Green; out.label = "Pokemon Green"; }
    else return true;

    std::vector<std::string> notes;
    uint8_t check = 0;
    for (size_t i = 0x134; i <= 0x14C; ++i) check = static_cast<uint8_t>(check - head[i] - 1);
    if (check != head[0x14D]) notes.push_back("header checksum mismatch");
    if (head[0x148] <= 8 && size != (uint64_t(0x8000) << head[0x148])) notes.push_back("unexpected file size");
    if (head[0x14A] == 0) notes.push_back("Japanese");
    for (size_t i = 0; i < notes.size(); ++i) out.note += (i ? ", " : "") + notes[i];
    return true;
}

bool inspect_save(const fs::path& path, Save& out) {
    std::vector<uint8_t> data;
    uint64_t size = 0;
    if (!read_head(path, data, kSaveSize + 1, &size)) return false;
    out = Save{};
    out.path = path;
    if (size != kSaveSize) { out.note = "size " + std::to_string(size) + " (expected 32768)"; return true; }
    uint32_t sum = 0;
    for (size_t i = kChecksumFirst; i <= kChecksumLast; ++i) sum += data[i];
    if (static_cast<uint8_t>(~(sum & 0xFF)) != data[kChecksumAt]) { out.note = "no Gen-1 save data (empty or another game)"; return true; }
    out.valid = true;
    for (size_t i = kChecksumFirst; i < kChecksumFirst + 11; ++i) {
        if (data[i] == 0x50 || data[i] == 0x00) break;
        const char c = gen1_char(data[i]);
        if (c != '\0') out.trainer.push_back(c);
    }
    char id[8];
    std::snprintf(id, sizeof(id), "%05d", (data[0x2605] << 8) | data[0x2606]);
    out.id = id;
    return true;
}

Skip default_skip() {
    return Skip{{"nintendo", "atmosphere", "bootloader", "emummc", "config", "contents", "themes", "retroarch",
                 ".overlays", ".packages", "appstore", "system volume information", "$recycle.bin"}};
}

Catalog scan(const std::vector<Root>& roots, const Skip& skip, size_t max_entries) {
    Catalog catalog;
    std::vector<fs::path> seen_roms, seen_saves;
    auto have = [](const std::vector<fs::path>& v, const fs::path& p) { return std::find(v.begin(), v.end(), p) != v.end(); };

    struct Walker {
        const Skip& skip;
        Catalog& catalog;
        size_t max_entries;
        std::vector<fs::path>& roms;
        std::vector<fs::path>& saves;
        bool (*have)(const std::vector<fs::path>&, const fs::path&);

        void visit(const fs::path& dir, int depth_left) {
            std::error_code ec;
            fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
            if (ec) return;
            std::vector<fs::path> subdirs;
            for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (catalog.files_seen >= max_entries) { catalog.truncated = true; return; }
                ++catalog.files_seen;
                const fs::path p = it->path();
                std::error_code e2;
                if (it->is_directory(e2)) {
                    const std::string name = lower(p.filename().string());
                    if (depth_left > 0 && std::find(skip.dir_names.begin(), skip.dir_names.end(), name) == skip.dir_names.end())
                        subdirs.push_back(p);
                    continue;
                }
                const std::string ext = ext_lower(p);
                if (ext == ".gb" || ext == ".gbc") {
                    if (have(roms, p)) continue;
                    roms.push_back(p);
                    Rom rom;
                    if (inspect_rom(p, rom)) {
                        if (rom.kind == Kind::Unknown) ++catalog.other_gb_files;
                        else catalog.roms.push_back(rom);
                    }
                } else if (ext == ".sav" || ext == ".srm") {
                    if (have(saves, p)) continue;
                    saves.push_back(p);
                    Save save;
                    if (inspect_save(p, save)) catalog.saves.push_back(save);
                }
            }
            std::sort(subdirs.begin(), subdirs.end());
            for (const fs::path& sub : subdirs) {
                if (catalog.truncated) return;
                visit(sub, depth_left - 1);
            }
        }
    } walker{skip, catalog, max_entries, seen_roms, seen_saves, +have};

    for (const Root& root : roots) walker.visit(root.path, root.max_depth);
    std::sort(catalog.roms.begin(), catalog.roms.end(), [](const Rom& a, const Rom& b) { return a.path < b.path; });
    std::sort(catalog.saves.begin(), catalog.saves.end(), [](const Save& a, const Save& b) { return a.path < b.path; });
    return catalog;
}

std::vector<size_t> save_candidates(const Catalog& catalog, const Rom& rom) {
    std::vector<size_t> order(catalog.saves.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return score_save(rom.path, catalog.saves[a]) < score_save(rom.path, catalog.saves[b]);
    });
    return order;
}

std::string display_path(const fs::path& path, const fs::path& base) {
    std::error_code ec;
    const fs::path rel = path.lexically_relative(base);
    if (!rel.empty() && *rel.begin() != "..") return rel.generic_string();
    return path.generic_string();
}

// "sdmc:" for "sdmc:/switch/x", "" for an ordinary path. std::filesystem calls "sdmc:/roms/x.gb" relative (no leading '/'),
// and transfer_pak.cpp prefixes every relative path with the config folder, so the config must not contain such paths.
std::string device_prefix(const std::string& path) {
    const size_t colon = path.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= path.size() || path[colon + 1] != '/') return {};
    for (size_t i = 0; i < colon; ++i) {
        if (!std::isalnum(static_cast<unsigned char>(path[i]))) return {};
    }
    return path.substr(0, colon + 1);
}

// How a path is written to launcher.cfg: relative to the config folder when inside it, otherwise rooted ("/roms/x.gb"),
// which libnx resolves on the current device (the program chdir()s to the data folder on the SD card first).
std::string config_path(const fs::path& path, const fs::path& base) {
    std::string text = display_path(path, base);
    const std::string device = device_prefix(base.generic_string());
    if (!device.empty() && device_prefix(text) == device) text.erase(0, device.size());
    return text;
}

bool normalize_config(const fs::path& cfg, const std::string& device) {
    std::vector<std::string> lines;
    bool changed = false;
    {
        std::ifstream in(cfg);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            std::string probe = line.substr(0, line.find_first_of("#;"));
            const size_t eq = probe.find('=');
            if (eq != std::string::npos) {
                const std::string key = lower(trim(probe.substr(0, eq)));
                const std::string value = trim(probe.substr(eq + 1));
                if (key.size() >= 6 && key[0] == 'p' && key[1] >= '1' && key[1] <= '4' && key[2] == '_' &&
                    device_prefix(value) == device) {
                    line = line.substr(0, line.find('=') + 1) + value.substr(device.size());
                    changed = true;
                }
            }
            lines.push_back(line);
        }
    }
    if (!changed) return false;
    std::error_code ec;
    fs::copy_file(cfg, fs::path(cfg).concat(".bak"), fs::copy_options::overwrite_existing, ec);
    std::ofstream out(cfg, std::ios::trunc);
    for (const std::string& l : lines) out << l << "\n";
    return static_cast<bool>(out);
}

Slots read_slots(const fs::path& cfg) {
    Slots slots;
    std::ifstream file(cfg);
    if (!file) return slots;
    const fs::path base = cfg.parent_path();
    std::string line;
    while (std::getline(file, line)) {
        const size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) line.resize(comment);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        if (value.empty() || key.size() < 6 || key[0] != 'p' || key[1] < '1' || key[1] > '4' || key[2] != '_') continue;
        Slot& slot = slots[key[1] - '1'];
        std::string text = value;
        const std::string device = device_prefix(base.generic_string());
        if (!device.empty() && text[0] == '/') text = device + text;   // rooted: same device as the config folder
        fs::path p(text);
        if (device_prefix(text).empty() && p.is_relative()) p = base / p;
        if (key.substr(3) == "rom") slot.rom = p;
        else if (key.substr(3) == "save") slot.save = p;
    }
    return slots;
}

bool write_slots(const fs::path& cfg, const Slots& slots, const fs::path& base) {
    std::vector<std::string> keep;
    {
        std::ifstream in(cfg);
        std::string line;
        while (in && std::getline(in, line)) {
            std::string probe = line;
            const size_t comment = probe.find_first_of("#;");
            if (comment != std::string::npos) probe.resize(comment);
            const size_t eq = probe.find('=');
            if (eq != std::string::npos) {
                const std::string key = lower(trim(probe.substr(0, eq)));
                if (key.size() >= 6 && key[0] == 'p' && key[1] >= '1' && key[1] <= '4' && key[2] == '_' &&
                    (key.substr(3) == "rom" || key.substr(3) == "save")) {
                    continue;  // replaced below
                }
            }
            keep.push_back(line);
        }
    }
    while (!keep.empty() && trim(keep.back()).empty()) keep.pop_back();

    const fs::path tmp = fs::path(cfg).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        if (keep.empty()) {
            out << "# Osaka-NX for Switch. Game Boy cartridges for the Transfer Pak / GB Tower.\n"
                << "# Written by the cartridge picker (hold L while the game starts to open it again). Paths are relative to this folder.\n";
        } else {
            for (const std::string& l : keep) out << l << "\n";
        }
        out << "\n";
        for (int i = 0; i < 4; ++i) {
            if (slots[i].rom.empty()) continue;
            out << "p" << i + 1 << "_rom=" << config_path(slots[i].rom, base) << "\n";
            if (!slots[i].save.empty()) out << "p" << i + 1 << "_save=" << config_path(slots[i].save, base) << "\n";
        }
        if (!out) return false;
    }
    std::error_code ec;
    if (fs::exists(cfg, ec)) {  // keep the previous file as launcher.cfg.bak
        fs::copy_file(cfg, fs::path(cfg).concat(".bak"), fs::copy_options::overwrite_existing, ec);
    }
    fs::rename(tmp, cfg, ec);
    if (ec) {  // some SD filesystems refuse to rename over an existing file
        fs::remove(cfg, ec);
        fs::rename(tmp, cfg, ec);
    }
    return !ec;
}

std::string describe(const Catalog& catalog, const Slot& slot, const fs::path& base) {
    if (slot.empty()) return "--- empty ---";
    Rom rom;
    bool found = false;
    for (const Rom& r : catalog.roms) if (r.path == slot.rom) { rom = r; found = true; break; }
    if (!found && !inspect_rom(slot.rom, rom)) return "ROM missing: " + display_path(slot.rom, base);
    std::string text = rom.label.empty() ? "(not a Gen-1 Pokemon ROM)" : rom.label;
    if (slot.save.empty()) return text + "  [no save]";
    Save save;
    bool have_save = false;
    for (const Save& s : catalog.saves) if (s.path == slot.save) { save = s; have_save = true; break; }
    if (!have_save) have_save = inspect_save(slot.save, save);
    if (!have_save) return text + "  [save missing: " + display_path(slot.save, base) + "]";
    if (save.valid) return text + "  " + save.trainer + " (ID " + save.id + ")";
    return text + "  [save: " + save.note + "]";
}


namespace {
    std::string short_game(const std::string& label) {   // "Pokemon Red" -> "Red"
        const std::string prefix = "Pokemon ";
        return label.compare(0, prefix.size(), prefix) == 0 ? label.substr(prefix.size()) : label;
    }
}

PortView port_view(const Catalog& catalog, const Slot& slot, const fs::path& base) {
    PortView v;
    if (slot.empty()) return v;
    v.used = true;
    v.rom_file = display_path(slot.rom, base);
    Rom rom;
    bool found = false;
    for (const Rom& r : catalog.roms) if (r.path == slot.rom) { rom = r; found = true; break; }
    if (!found && !inspect_rom(slot.rom, rom)) { v.name = "ROM missing"; v.problem = true; return v; }
    v.kind = rom.kind;
    v.game = short_game(rom.label);
    if (slot.save.empty()) { v.name = "No save"; return v; }
    v.save_file = display_path(slot.save, base);
    Save save;
    bool have = false;
    for (const Save& s : catalog.saves) if (s.path == slot.save) { save = s; have = true; break; }
    if (!have) have = inspect_save(slot.save, save);
    if (!have) { v.name = "Save missing"; v.problem = true; return v; }
    if (!save.valid) { v.name = "Bad save"; v.problem = true; return v; }
    v.name = save.trainer;
    std::string id = save.id;
    while (id.size() < 5) id.insert(id.begin(), '0');
    v.id = "ID " + id;
    return v;
}

// ---- screens ---------------------------------------------------------------------------------------------------------
namespace {
    enum class Screen { Main, PickRom, PickSave, Message };

    std::string clip(const std::string& s, size_t width) {
        return s.size() <= width ? s : s.substr(0, width - 3) + "...";
    }

    // Appends the visible window of a list, cursor marked with '>'.
    void list_window(std::vector<std::string>& lines, const std::vector<std::string>& items, size_t cursor, size_t rows) {
        size_t first = 0;
        if (cursor >= rows / 2) first = cursor - rows / 2;
        if (first + rows > items.size()) first = items.size() > rows ? items.size() - rows : 0;
        if (first > 0) lines.push_back("   ... " + std::to_string(first) + " more above");
        for (size_t i = first; i < items.size() && i < first + rows; ++i) lines.push_back((i == cursor ? " > " : "   ") + items[i]);
        if (first + rows < items.size()) lines.push_back("   ... " + std::to_string(items.size() - first - rows) + " more below");
    }
}

Result run(Frontend& ui, const Catalog& catalog, Slots& slots, const fs::path& cfg, const fs::path& base) {
    constexpr size_t kWidth = 76, kRows = 24;
    Screen screen = Screen::Main;
    size_t port = 0;                // main-screen cursor
    size_t cursor = 0;              // list cursor
    int edit = -1;                  // port being edited
    std::vector<size_t> candidates; // save list entries (indices into catalog.saves; last entry = "no save")
    std::string message, notice;
    Screen after_message = Screen::Main;
    bool changed = false;

    auto start_pick_save = [&](int p) {
        Rom rom;
        if (!inspect_rom(slots[p].rom, rom)) { screen = Screen::Main; return; }
        candidates = save_candidates(catalog, rom);
        edit = p;
        cursor = 0;
        for (size_t i = 0; i < candidates.size(); ++i)
            if (catalog.saves[candidates[i]].path == slots[p].save) cursor = i;
        screen = Screen::PickSave;
    };

    for (;;) {
        std::vector<std::string> lines;
        std::vector<std::string> items;
        View view;
        view.found_roms = static_cast<int>(catalog.roms.size());
        view.found_saves = static_cast<int>(catalog.saves.size());
        for (size_t i = 0; i < 4; ++i) view.ports[i] = port_view(catalog, slots[i], base);
        switch (screen) {
        case Screen::Main: {
            lines.push_back(" POKEMON STADIUM - GAME BOY CARTRIDGES (Transfer Pak / GB Tower)");
            lines.push_back("");
            lines.push_back(" Found " + std::to_string(catalog.roms.size()) + " Pokemon ROM(s) and " + std::to_string(catalog.saves.size()) +
                            " save file(s)" + (catalog.truncated ? " (search stopped early: too many files)" : "") + ".");
            lines.push_back("");
            for (size_t i = 0; i < 4; ++i) {
                lines.push_back((i == port ? " > " : "   ") + std::string("Port ") + std::to_string(i + 1) + "   " +
                                clip(describe(catalog, slots[i], base), kWidth - 14));
                if (!slots[i].empty()) {
                    lines.push_back("             " + clip(display_path(slots[i].rom, base), kWidth - 14));
                    if (!slots[i].save.empty()) lines.push_back("             " + clip(display_path(slots[i].save, base), kWidth - 14));
                }
            }
            lines.push_back("");
            for (size_t i = 0; i < 4; ++i)
                for (size_t j = i + 1; j < 4; ++j)
                    if (!slots[i].save.empty() && slots[i].save == slots[j].save)
                        lines.push_back(" !! Ports " + std::to_string(i + 1) + " and " + std::to_string(j + 1) + " use the same save file.");
            if (!notice.empty()) lines.push_back(" " + notice);
            lines.push_back(" Port N is the Nth controller you connect to the console.");
            lines.push_back("");
            lines.push_back(" [Up/Left, Down/Right] port   [A] choose cartridge   [X] change save only   [Y] clear");
            lines.push_back(" [+] SAVE AND START GAME   [B] start without changes");
            view.type = View::Type::Main;
            view.port = static_cast<int>(port);
            for (size_t i = 0; i < 4; ++i)
                for (size_t j = i + 1; j < 4; ++j)
                    if (!slots[i].save.empty() && slots[i].save == slots[j].save)
                        view.notices.push_back("Ports " + std::to_string(i + 1) + " and " + std::to_string(j + 1) + " use the same save file.");
            if (!notice.empty()) view.notices.push_back(notice);
            view.hints = {{"A", "Cartridge"}, {"X", "Save"}, {"Y", "Clear"}, {"+", "Start"}, {"B", "Skip"}};
            break;
        }
        case Screen::PickRom: {
            lines.push_back(" Port " + std::to_string(edit + 1) + ": choose a cartridge");
            lines.push_back("");
            for (const Rom& r : catalog.roms) {
                std::string text = r.label + "   " + display_path(r.path, base);
                if (!r.note.empty()) text += "  (" + r.note + ")";
                items.push_back(clip(text, kWidth));
            }
            list_window(lines, items, cursor, kRows);
            lines.push_back("");
            lines.push_back(" [Up/Left, Down/Right] move   [A] select   [B] back");
            view.type = View::Type::PickRom;
            view.port = edit;
            view.cursor = static_cast<int>(cursor);
            view.subtitle = "Port " + std::to_string(edit + 1);
            for (const Rom& r : catalog.roms) {
                view.items.push_back(short_game(r.label) + (r.note.empty() ? "" : " (" + r.note + ")"));
                view.details.push_back(display_path(r.path, base));
            }
            view.hints = {{"A", "Select"}, {"B", "Back"}};
            break;
        }
        case Screen::PickSave: {
            Rom rom;
            inspect_rom(slots[edit].rom, rom);
            lines.push_back(" Port " + std::to_string(edit + 1) + ": choose the save for " + rom.label);
            lines.push_back(" " + clip(display_path(rom.path, base), kWidth));
            lines.push_back("");
            for (size_t c : candidates) {
                const Save& s = catalog.saves[c];
                std::string text = display_path(s.path, base);
                text = clip(text, 40);
                text += s.valid ? "   " + s.trainer + " (ID " + s.id + ")" : "   [" + s.note + "]";
                items.push_back(clip(text, kWidth));
            }
            items.push_back("(no save: start without saved data)");
            list_window(lines, items, cursor, kRows);
            lines.push_back("");
            lines.push_back(" [Up/Left, Down/Right] move   [A] select   [B] back");
            view.type = View::Type::PickSave;
            view.port = edit;
            view.cursor = static_cast<int>(cursor);
            view.subtitle = "Port " + std::to_string(edit + 1) + ": " + short_game(rom.label);
            for (size_t c : candidates) {
                const Save& sv = catalog.saves[c];
                view.items.push_back(sv.valid ? sv.trainer : "Bad save");
                std::string id = sv.id;
                while (!id.empty() && id.size() < 5) id.insert(id.begin(), '0');
                view.details.push_back((sv.valid ? "ID " + id + "  " : "(" + sv.note + ")  ") + display_path(sv.path, base));
            }
            view.items.push_back("No save");
            view.details.push_back("Start without saved data");
            view.hints = {{"A", "Select"}, {"B", "Back"}};
            break;
        }
        case Screen::Message:
            lines.push_back("");
            lines.push_back(" " + message);
            lines.push_back("");
            lines.push_back(" [A] ok");
            view.type = View::Type::Message;
            view.message = message;
            view.hints = {{"A", "OK"}};
            break;
        }
        ui.draw_view(view, lines);

        Key key;
        if (!ui.wait_key(key)) return Result::Quit;
        // Left/Right move the cursor the same way as Up/Down (Left = previous, Right = next), on every screen.
        if (key == Key::Left) key = Key::Up;
        else if (key == Key::Right) key = Key::Down;

        switch (screen) {
        case Screen::Main:
            notice.clear();
            if (key == Key::Up && port > 0) --port;
            else if (key == Key::Down && port < 3) ++port;
            else if (key == Key::A) {
                if (catalog.roms.empty()) {
                    message = "No Pokemon Red/Blue/Yellow ROMs (.gb/.gbc) found. Copy yours to the 'gb' folder in switch/osaka-nx on the SD card.";
                    after_message = Screen::Main;
                    screen = Screen::Message;
                } else {
                    edit = static_cast<int>(port);
                    cursor = 0;
                    for (size_t i = 0; i < catalog.roms.size(); ++i) if (catalog.roms[i].path == slots[port].rom) cursor = i;
                    screen = Screen::PickRom;
                }
            } else if (key == Key::X) {
                if (slots[port].empty()) notice = "Choose a cartridge for this port first.";
                else start_pick_save(static_cast<int>(port));
            } else if (key == Key::Y) {
                if (!slots[port].empty()) { slots[port] = Slot{}; changed = true; }
            } else if (key == Key::Plus) {
                if (changed && !write_slots(cfg, slots, base)) {
                    ui.draw({"", " Could not write " + cfg.generic_string() + ".", " The new choices apply to this session only.", "", " [A] ok"});
                    Key ack;
                    if (!ui.wait_key(ack)) return Result::Quit;
                }
                return Result::Start;
            } else if (key == Key::B || key == Key::Minus) {
                return Result::Skip;  // start with launcher.cfg as it was
            }
            break;
        case Screen::PickRom:
            if (key == Key::Up && cursor > 0) --cursor;
            else if (key == Key::Down && cursor + 1 < catalog.roms.size()) ++cursor;
            else if (key == Key::B) screen = Screen::Main;
            else if (key == Key::A) {
                const Rom& chosen = catalog.roms[cursor];
                const bool same = slots[edit].rom == chosen.path;
                slots[edit].rom = chosen.path;
                if (!same) slots[edit].save.clear();
                changed = true;
                const auto cands = save_candidates(catalog, chosen);
                if (cands.empty()) {
                    slots[edit].save.clear();
                    notice = "No save file found for " + chosen.label + ": it will run without saved data.";
                    screen = Screen::Main;
                } else if (same && !slots[edit].save.empty()) {
                    screen = Screen::Main;  // same cartridge picked again: keep its save
                } else if (score_save(chosen.path, catalog.saves[cands[0]]) <= 1 &&
                           (cands.size() == 1 || score_save(chosen.path, catalog.saves[cands[1]]) > 1)) {
                    slots[edit].save = catalog.saves[cands[0]].path;  // one save with the ROM's own name: take it
                    screen = Screen::Main;
                } else {
                    start_pick_save(edit);
                }
            }
            break;
        case Screen::PickSave:
            if (key == Key::Up && cursor > 0) --cursor;
            else if (key == Key::Down && cursor < candidates.size()) ++cursor;  // + the "no save" row
            else if (key == Key::B) screen = Screen::Main;
            else if (key == Key::A) {
                slots[edit].save = cursor < candidates.size() ? catalog.saves[candidates[cursor]].path : fs::path{};
                changed = true;
                screen = Screen::Main;
            }
            break;
        case Screen::Message:
            screen = after_message;
            break;
        }
    }
}
}
