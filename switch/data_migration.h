// One-time move of the data folder from the pre-release name to the Osaka-NX name.
//
// Background: before the project was renamed, its folder on the SD card was sdmc:/switch/pokestadiumrecomp and its settings file
// was psrnx.cfg. So that nobody has to move the ROM, saves, launcher.cfg and learned shader lists by hand, the first start of
// the renamed program renames the old folder to the new name.
//
// What it does, in order, and nothing else:
//   1. old folder missing                       -> does nothing (a fresh install, or already migrated).
//   2. new folder does not exist                -> renames old folder -> new folder (a rename only: nothing is copied or deleted).
//   3. new folder already exists                -> MERGES: every file or folder of the old folder that has no counterpart in the new
//      one is moved over (saves/, ROM.z64, roms/, ...). Folders present in both are merged the same way, one level at a time.
//      A file present in BOTH is never replaced: the new one stays and the old one stays where it was (reported as "kept").
//      *.nro files are not moved (the old program file is of no use under the new name).
//      This case is the usual one: installing osaka-nx.nro into sdmc:/switch/osaka-nx/ creates the new folder before the first
//      start, so the plain rename of step 2 would never have happened.
//   4. inside the new folder: psrnx.cfg -> osaka-nx.cfg, unless osaka-nx.cfg already exists.
//   5. launcher.cfg: any absolute path that names the old folder is rewritten to name the new one. (Paths the cartridge picker
//      wrote are relative to the folder and need no change.)
// Running it again is harmless: once everything has moved there is nothing left to do and the result is empty.
//
// The functions take the folder names as arguments so the host test (tests/migrate_test.cpp) can run them on a temporary folder.
// Header-only and portable C++17 (it only needs <sys/stat.h>, <cstdio> and file streams).
#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <sys/stat.h>

namespace psrnx_migrate {
    // True when something (file or folder) exists at `path`.
    inline bool exists(const std::string& path) {
        struct stat st;
        return stat(path.c_str(), &st) == 0;
    }

    // Replaces every occurrence of `from` by `to` in the text file at `path`. Returns true if the file was changed.
    inline bool rewrite_paths(const std::string& path, const std::string& from, const std::string& to) {
        std::string text;
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        bool changed = false;
        for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size())) {
            text.replace(pos, from.size(), to);
            changed = true;
        }
        if (changed) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << text;
        }
        return changed;
    }

    // Moves every entry of `src` that has no counterpart in `dst` into `dst`; recurses into folders present in both.
    // Counts what happened: `moved` entries moved, `kept` files that exist on both sides (the new one wins, nothing is changed).
    // `top` is true only for the first level, where *.nro program files are skipped.
    inline void merge_folder(const std::string& src, const std::string& dst, bool top, int& moved, int& kept) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(dst, ec);
        std::vector<fs::path> entries;
        for (fs::directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(it->path());
        for (const fs::path& from : entries) {
            const std::string name = from.filename().string();
            if (top && name.size() > 4 && name.compare(name.size() - 4, 4, ".nro") == 0) continue;
            const std::string to = dst + "/" + name;
            const bool from_is_dir = fs::is_directory(from, ec);
            if (!exists(to)) {
                if (std::rename(from.string().c_str(), to.c_str()) == 0) ++moved; else ++kept;
            } else if (from_is_dir && fs::is_directory(to, ec)) {
                merge_folder(from.string(), to, false, moved, kept);
            } else {
                ++kept;
            }
        }
    }

    // Runs the steps above. `old_dir` and `new_dir` have no trailing slash, `old_name` / `new_name` are the folder names as they
    // appear inside paths in launcher.cfg (for example "/switch/pokestadiumrecomp/" and "/switch/osaka-nx/").
    // Returns a one-line description of what happened, or an empty string when there was nothing to do.
    inline std::string migrate_data_folder(const std::string& old_dir, const std::string& new_dir, const std::string& old_path_in_cfg,
                                           const std::string& new_path_in_cfg) {
        if (!exists(old_dir)) return "";                                                                  // step 1
        std::string note;
        bool launcher_moved = false;
        if (!exists(new_dir)) {                                                                           // step 2
            if (std::rename(old_dir.c_str(), new_dir.c_str()) != 0)
                return "could not rename " + old_dir + " to " + new_dir + "; move it by hand";
            note = "moved the data folder " + old_dir + " -> " + new_dir;
            launcher_moved = true;
        } else {                                                                                          // step 3
            const bool had_launcher = exists(new_dir + "/launcher.cfg");
            int moved = 0, kept = 0;
            merge_folder(old_dir, new_dir, true, moved, kept);
            if (moved == 0 && kept == 0) return "";
            if (moved > 0) note = "moved " + std::to_string(moved) + " item(s) from " + old_dir + " into " + new_dir;
            if (kept > 0) note += std::string(note.empty() ? "" : "; ") + "kept " + std::to_string(kept) +
                                  " file(s) that exist in both places (the ones in " + new_dir + " are used; the old ones stay in " + old_dir + ")";
            launcher_moved = !had_launcher && exists(new_dir + "/launcher.cfg");
        }
        if (exists(new_dir + "/psrnx.cfg") && !exists(new_dir + "/osaka-nx.cfg") &&                       // step 4
            std::rename((new_dir + "/psrnx.cfg").c_str(), (new_dir + "/osaka-nx.cfg").c_str()) == 0)
            note += "; psrnx.cfg -> osaka-nx.cfg";
        if (launcher_moved && rewrite_paths(new_dir + "/launcher.cfg", old_path_in_cfg, new_path_in_cfg)) // step 5
            note += "; launcher.cfg paths updated";
        return note;
    }
}
