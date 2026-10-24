// Optional sdmc:/switch/osaka-nx/osaka-nx.cfg: "key=value" lines, '#' comments.
//   resolution_factor=2|3      (see psrnx.h)
//   widescreen=0|1|2           (0 = 4:3 with bars, 1 = widescreen: 3D widened, menus stay 4:3 (experimental), 2 = stretch to 16:9 (default))
//   audio_buffer_frames=1024
//   interp_discovery=1|0       (1 = record every function the static recompile is missing into coverage/runtime_captures.json)
//   stall_boost=1|0            (1 = request the CPU boost clock while a loading stall is under way)
//   gb_picker=auto|always|never (Game Boy cartridge picker at start-up; hold L while starting to open it in auto mode)
//   picker_wait_ms=1500        (start-up prompt length for opening the picker with L/ZL/R/ZR/-; 0 = no prompt)
//   left_stick_dpad=1|0        (1 = the left stick also presses the D-pad, so it navigates menus; 0 = stick is analog only)
//   nintendo_buttons=1|0       (1 = Switch A confirms and Switch B cancels, as the labels on the pad say; 0 = A/B by position)
//   gl_copy_color=-1|0|1|2|3   (GLideN64 EnableCopyColorToRDRAM; -1 = GLideN64's own default (2, double buffer); default 0 = off, much faster page loads)
//   gl_swap_mode=-1|0|1|2      (GLideN64 BufferSwapMode; -1 = default (0, every VI))
//   vi_hold=150                (presents after a video-mode change during which unrendered RDRAM pictures are hidden; 0 = show them)
//   gbcart_ring=0|1            (1 = write gbcart_ring.txt, the Transfer Pak access log; freezes the game while it is written, diagnosis only)
//   log_buffered=1|0           (1 = log files go through big buffers emptied every 50 ms (no stall when the runtime prints hundreds of lines); 0 = one write per line)
//   hash_miss_dump=0|1         (1 = let the runtime write hash_miss_id_EF.bin at every unknown overlay (~0.3 s stall each); default 0)
//   gb_view=2                 (GB Tower screen: 0 = as drawn, 1 = bezel centred in Game Boy proportions, 2 = bezel stretched to fill, 3 = bezel in a centred 4:3 box)
//   vi_field=1|2|0            (480i menus: 1 = upper field (default, steady), 2 = lower field, 0 = GLideN64's alternation = jitter)
//   shader_warm_extra=1        (0/1/2: also build the shaders a ROM scan says the game may use: none / likely / all, see psrnx.h)
//   shader_prune=1|2|0         (start-up shaders: 1 = once 40 used keys are recorded (shader_used.txt) build only those, 2 = those + the built-in list, 0 = full list)
//   shader_opt=1               (nouveau shader optimisation level 0-3 while compiling, -1 = Mesa's default 3; lower = faster start-up)
//   splash=1|0                 (1 = "Please wait" picture with a progress bar while the shaders are built at start-up; 0 = black window)
//   shader_warm_ms=45000       (time budget for building known shaders at start-up, see psrnx.h; 0 = off, shaders build on demand = freezes)
//   vi_probe=0|1               (diagnosis: log how the presented picture moves between frames; slows the game)
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "psrnx.h"

namespace {
    psrnx::Config g_config;
    bool g_loaded = false;

    void load() {
        g_loaded = true;
        FILE* file = std::fopen(PSRNX_DATA_DIR "/osaka-nx.cfg", "r");
        if (file == nullptr) return;
        char line[256];
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            if (line[0] == '#') continue;
            char* eq = std::strchr(line, '=');
            if (eq == nullptr) continue;
            *eq = '\0';
            const int value = std::atoi(eq + 1);
            if (std::strcmp(line, "gb_picker") == 0) {
                const char* v = eq + 1;
                g_config.gb_picker = std::strncmp(v, "never", 5) == 0 ? 0 : std::strncmp(v, "always", 6) == 0 ? 2 : 1;
                continue;
            }
            if (std::strcmp(line, "resolution_factor") == 0 && value >= 1 && value <= 4) {
                g_config.resolution_factor = value;
            } else if (std::strcmp(line, "widescreen") == 0) {
                const char* v = eq + 1;
                g_config.widescreen = std::strncmp(v, "stretch", 7) == 0 ? 2
                                    : std::strncmp(v, "adjust", 6) == 0 || std::strncmp(v, "wide", 4) == 0 ? 1
                                    : std::strncmp(v, "4:3", 3) == 0 ? 0
                                    : (value >= 0 && value <= 2 ? value : g_config.widescreen);
            } else if (std::strcmp(line, "interp_discovery") == 0) {
                g_config.interp_discovery = value != 0;
            } else if (std::strcmp(line, "stall_boost") == 0) {
                g_config.stall_boost = value != 0;
            } else if (std::strcmp(line, "picker_wait_ms") == 0 && value >= 0 && value <= 10000) {
                g_config.picker_wait_ms = value;
            } else if (std::strcmp(line, "left_stick_dpad") == 0) {
                g_config.left_stick_dpad = value != 0;
            } else if (std::strcmp(line, "nintendo_buttons") == 0) {
                g_config.nintendo_buttons = value != 0;
            } else if (std::strcmp(line, "gl_copy_color") == 0 && value >= -1 && value <= 3) {
                g_config.gl_copy_color = value;
            } else if (std::strcmp(line, "vi_hold") == 0 && value >= 0 && value <= 240) {
                g_config.vi_hold = value;
            } else if (std::strcmp(line, "gbcart_ring") == 0) {
                g_config.gbcart_ring = value != 0;
            } else if (std::strcmp(line, "log_buffered") == 0) {
                g_config.log_buffered = value != 0;
            } else if (std::strcmp(line, "hash_miss_dump") == 0) {
                g_config.hash_miss_dump = value != 0;
            } else if (std::strcmp(line, "gb_view") == 0 && value >= 0 && value <= 3) {
                g_config.gb_view = value;
            } else if (std::strcmp(line, "gl_swap_mode") == 0 && value >= -1 && value <= 2) {
                g_config.gl_swap_mode = value;
            } else if (std::strcmp(line, "vi_field") == 0 && value >= 0 && value <= 2) {
                g_config.vi_field = value;
            } else if (std::strcmp(line, "shader_warm_extra") == 0 && value >= 0 && value <= 2) {
                g_config.shader_warm_extra = value;
            } else if (std::strcmp(line, "shader_prune") == 0 && value >= 0 && value <= 2) {
                g_config.shader_prune = value;
            } else if (std::strcmp(line, "shader_opt") == 0 && value >= -1 && value <= 3) {
                g_config.shader_opt = value;
            } else if (std::strcmp(line, "splash") == 0) {
                g_config.splash = value != 0;
            } else if (std::strcmp(line, "shader_warm_ms") == 0 && value >= 0 && value <= 120000) {
                g_config.shader_warm_ms = value;
            } else if (std::strcmp(line, "vi_probe") == 0) {
                g_config.vi_probe = value != 0;
            } else if (std::strcmp(line, "audio_buffer_frames") == 0 && value >= 256 && value <= 4096) {
                g_config.audio_buffer_frames = value;
            }
        }
        std::fclose(file);
    }
}

const psrnx::Config& psrnx::config() {
    if (!g_loaded) load();
    return g_config;
}

// The window the compositor gets. 720p in both handheld and docked mode: libnx scales the layer, and the cost of
// the game's render resolution is set by resolution_factor, not by the window.
int psrnx::screen_width() { return 1280; }
int psrnx::screen_height() { return 720; }
