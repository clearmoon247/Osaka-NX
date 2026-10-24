// Switch front end of the Game Boy cartridge picker (gbpicker.h), driven by the pads.
// Runs at the very start of main(), before the log is redirected and before any window or GL context exists, so it owns the
// screen and tears it down again before the game opens its window.
//
// Two looks: when the player's own Pokemon Stadium (USA) ROM is in the data folder, the picker is drawn like the game's
// "Game Pak Check" menu (gbpicker_gfx.cpp, with the font, panels and background read from that ROM by stadium_skin.cpp) on a
// libnx framebuffer; otherwise, or if the framebuffer cannot be created, it is a text screen on the libnx console.
//
// When it appears (osaka-nx.cfg `gb_picker=auto|always|never`, default auto):
//   - L, ZL, R, ZR or - held or pressed during the short prompt shown at start-up (any setting but never; hbmenu itself uses
//     L/ZL for paging, and the large NRO can take a while to hand over input, so a held button is not reliable on its own), or
//   - auto: there is no launcher.cfg yet and a Pokemon ROM was found on the SD card, or
//   - always.
// The logic underneath is covered by tests/gbpicker_test.cpp and tests/skin_test.cpp.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <switch.h>

#include "gbpicker.h"
#include "gbpicker_gfx.h"
#include "psrnx.h"
#include "stadium_skin.h"

namespace {
    // Pad input shared by both looks.
    class PadInput {
    public:
        PadInput() {
            padConfigureInput(8, HidNpadStyleSet_NpadStandard);
            padInitializeAny(&pad_);
        }

        // Buttons that open the picker at start-up. hbmenu uses L/ZL itself, so R, ZR and - work too; held or newly pressed.
        bool trigger_held() {
            padUpdate(&pad_);
            return (padGetButtons(&pad_) & (HidNpadButton_L | HidNpadButton_ZL | HidNpadButton_R | HidNpadButton_ZR | HidNpadButton_Minus)) != 0;
        }

        // Polls for the trigger for `ms` milliseconds. 1 = pressed, 0 = not pressed, -1 = the application is closing.
        // The pad may report nothing for the first frames after hbmenu hands over (the NRO is large and input is not delivered
        // until the application has focus), and the player may only press the button once they see the prompt.
        int wait_for_trigger(int ms) {
            for (int waited = 0; waited < ms; waited += 16) {
                if (!appletMainLoop()) return -1;
                if (trigger_held()) return 1;
                svcSleepThread(16'000'000);
            }
            return 0;
        }

        // One poll: true and `key` set when a button went down (or auto-repeats while a direction is held).
        bool poll(gbpicker::Key& key) {
            using gbpicker::Key;
            padUpdate(&pad_);
            const u64 down = padGetButtonsDown(&pad_);
            const u64 held = padGetButtons(&pad_);
            // Auto-repeat for scrolling a long list: after half a second held, every 5th frame.
            const u64 scroll = HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_StickLUp |
                               HidNpadButton_StickLDown | HidNpadButton_StickLLeft | HidNpadButton_StickLRight;
            if (held & scroll) ++held_frames_; else held_frames_ = 0;
            u64 pressed = down;
            if (held_frames_ > 30 && held_frames_ % 5 == 0) pressed |= held & scroll;

            struct Map { u64 mask; Key key; };
            static const Map map[] = {
                {HidNpadButton_Up | HidNpadButton_StickLUp, Key::Up},
                {HidNpadButton_Down | HidNpadButton_StickLDown, Key::Down},
                {HidNpadButton_Left | HidNpadButton_StickLLeft, Key::Left},
                {HidNpadButton_Right | HidNpadButton_StickLRight, Key::Right},
                {HidNpadButton_A, Key::A},
                {HidNpadButton_B, Key::B},
                {HidNpadButton_X, Key::X},
                {HidNpadButton_Y, Key::Y},
                {HidNpadButton_Plus, Key::Plus},
                {HidNpadButton_Minus, Key::Minus},
            };
            for (const Map& m : map) {
                if (pressed & m.mask) { key = m.key; return true; }
            }
            return false;
        }

    private:
        PadState pad_{};
        int held_frames_ = 0;
    };

    // Text screen on the libnx console (the plain fallback).
    class ConsoleFrontend : public gbpicker::Frontend {
    public:
        explicit ConsoleFrontend(PadInput& pad) : pad_(pad) { consoleInit(nullptr); }
        ~ConsoleFrontend() override { consoleExit(nullptr); }

        void draw(const std::vector<std::string>& lines) override {
            consoleClear();
            for (const std::string& line : lines) std::printf("%s\n", line.c_str());
            consoleUpdate(nullptr);
        }

        bool wait_key(gbpicker::Key& key) override {
            while (appletMainLoop()) {
                if (pad_.poll(key)) return true;
                consoleUpdate(nullptr);  // waits for vsync, so this loop runs at 60 Hz
            }
            return false;
        }

        void notice(const char* text) { consoleClear(); std::printf("\n %s\n", text); consoleUpdate(nullptr); }

    private:
        PadInput& pad_;
    };

    // The Game Pak Check look: the screen is composed once per change into a 1280x720 RGBA buffer and copied to both
    // framebuffer images, so waiting for a button costs nothing.
    class GraphicalFrontend : public gbpicker::Frontend {
    public:
        GraphicalFrontend(PadInput& pad, const skin::Skin& skin) : pad_(pad), gfx_(skin), out_(static_cast<size_t>(kW) * kH) {
            if (R_SUCCEEDED(framebufferCreate(&fb_, nwindowGetDefault(), kW, kH, PIXEL_FORMAT_RGBA_8888, 2))) {
                framebufferMakeLinear(&fb_);
                open_ = true;
            }
        }
        ~GraphicalFrontend() override { if (open_) framebufferClose(&fb_); }
        bool ok() const { return open_; }

        void draw(const std::vector<std::string>&) override {}  // never used: draw_view is overridden

        void draw_view(const gbpicker::View& view, const std::vector<std::string>&) override {
            gfx_.render(view);
            gfx_.present(out_.data(), kW, kH, kW, psrnx::config().widescreen == 2);
            show();
        }

        void notice(const char* text) {
            gbpicker::View v;
            v.type = gbpicker::View::Type::Message;
            v.message = text;
            draw_view(v, {});
        }

        bool wait_key(gbpicker::Key& key) override {
            while (appletMainLoop()) {
                if (pad_.poll(key)) return true;
                svcSleepThread(16'000'000);
            }
            return false;
        }

    private:
        static constexpr int kW = 1280, kH = 720;

        void show() {
            for (int image = 0; image < 2; ++image) {  // both buffers of the swap chain
                u32 stride = 0;
                void* buf = framebufferBegin(&fb_, &stride);
                for (int y = 0; y < kH; ++y)
                    std::memcpy(static_cast<u8*>(buf) + static_cast<size_t>(y) * stride, &out_[static_cast<size_t>(y) * kW], static_cast<size_t>(kW) * 4);
                framebufferEnd(&fb_);
            }
        }

        PadInput& pad_;
        gbpicker::Gfx gfx_;
        std::vector<uint32_t> out_;
        Framebuffer fb_{};
        bool open_ = false;
    };
}

namespace {
    // Scan, show the picker, report. `Ui` is either front end above. Returns false when the application is closing.
    template <class Ui>
    bool run_flow(Ui& ui, bool forced, std::string& log) {
        namespace gp = gbpicker;
        namespace fs = gbpicker::fs;
        const fs::path base = PSRNX_DATA_DIR;
        const fs::path cfg = base / "launcher.cfg";

        ui.notice("Looking for Pokemon Game Boy cartridges on the SD card...");
        const gp::Skip skip = gp::default_skip();
        // Where to look, most likely first (the walk stops after 60000 entries, so the order matters): the data folder, then
        // sdmc:/roms, then the whole card three levels deep. That includes /switch, so cartridge files and saves kept in any
        // homebrew folder (for example the data folder of the pre-release build) are found; only the folders in
        // gbpicker::default_skip() (system data, app caches) are left out.
        const gp::Catalog catalog = gp::scan({{base, 3}, {"sdmc:/roms", 4}, {"sdmc:/", 3}}, skip, 60000);
        char summary[160];
        std::snprintf(summary, sizeof(summary), "; %zu ROM(s), %zu save(s), %zu other .gb files, %zu entries%s",
                      catalog.roms.size(), catalog.saves.size(), catalog.other_gb_files, catalog.files_seen,
                      catalog.truncated ? " (search stopped early)" : "");
        log += summary;

        if (!forced && catalog.roms.empty()) {  // nothing to offer on a first run: go straight to the game
            log += "; nothing to pick";
            return true;
        }

        gp::Slots slots = gp::read_slots(cfg);
        const gp::Result result = gp::run(ui, catalog, slots, cfg, base);
        switch (result) {
        case gp::Result::Start: log += "; saved choices"; break;
        case gp::Result::Skip:  log += "; skipped"; break;
        case gp::Result::Quit:  log += "; quit"; return false;
        }
        return true;
    }
}

namespace {
    // After the UI is up: when launcher.cfg already exists, show a short prompt and give the player a moment to ask for the
    // picker; otherwise (first run, forced) go straight in. Returns false when the application is closing.
    template <class Ui>
    bool run_ui(Ui& ui, PadInput& pad, bool forced, bool have_cfg, bool repaired, std::string& log) {
        if (!forced && have_cfg) {
            const int wait_ms = psrnx::config().picker_wait_ms;
            int answer = 0;
            if (wait_ms > 0) {
                ui.notice("Starting the game...  Press L, R or - now to change your Game Paks.");
                answer = pad.wait_for_trigger(wait_ms);
            }
            if (answer < 0) { log += "; quit"; return false; }
            if (answer == 0) {
                log += repaired ? "; skipped (launcher.cfg exists, rewrote sdmc:/ paths)" : "; skipped (launcher.cfg exists)";
                return true;
            }
            forced = true;
        }
        return run_flow(ui, forced, log);
    }
}

// Returns false when the application is being closed. `log` receives a line for osaka-nx.log (the log file is
// not open yet when this runs).
bool psrnx_run_gb_picker(std::string& log) {
    namespace gp = gbpicker;
    namespace fs = gbpicker::fs;
    const int mode = psrnx::config().gb_picker;  // 0 never, 1 auto, 2 always
    const fs::path base = PSRNX_DATA_DIR;
    const fs::path cfg = base / "launcher.cfg";
    std::error_code ec;
    if (mode == 0) {
        const bool fixed = fs::exists(cfg, ec) && gp::normalize_config(cfg, "sdmc:");
        log = fixed ? "gb picker: disabled (gb_picker=never); rewrote sdmc:/ paths in launcher.cfg" : "gb picker: disabled (gb_picker=never)";
        return true;
    }
    if (!fs::is_directory(base, ec)) { log = "gb picker: data folder not created yet"; return true; }

    PadInput pad;
    const bool forced = mode == 2 || pad.trigger_held();   // already held at the very first frame: no prompt needed
    const bool have_cfg = fs::exists(cfg, ec);
    // Earlier hand-written or picker-written configs may hold "sdmc:/..." paths, which transfer_pak.cpp would treat as relative.
    const bool repaired = have_cfg && gp::normalize_config(cfg, "sdmc:");

    // The game's own look needs the player's ROM; without it (or with a different dump) the text screen is used.
    skin::Skin look;
    const bool skinned = skin::load_from_folder(base, look);
    if (skinned) {
        GraphicalFrontend ui(pad, look);
        if (ui.ok()) {
            log = "gb picker: Game Pak Check look";
            return run_ui(ui, pad, forced, have_cfg, repaired, log);
        }
        log = "gb picker: framebuffer unavailable, text screen";
    } else {
        log = "gb picker: text screen (" + look.error + ")";
    }
    ConsoleFrontend ui(pad);
    return run_ui(ui, pad, forced, have_cfg, repaired, log);
}
