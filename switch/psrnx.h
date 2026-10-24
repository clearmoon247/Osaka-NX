// Shared constants and the settings structure of Osaka-NX (settings file: sdmc:/switch/osaka-nx/osaka-nx.cfg).
// "psrnx" is the project's old working name; it survives in identifiers and in the "[PSRNX]" prefix of log lines.
#pragma once

#include <atomic>
#include <cstdint>

#define PSRNX_DATA_DIR "sdmc:/switch/osaka-nx"

namespace psrnx {
    // Values read from sdmc:/switch/osaka-nx/osaka-nx.cfg (key=value lines) at start-up; the defaults are the
    // conservative bring-up settings. See psrnx_config.cpp.
    struct Config {
        // GLideN64 UseNativeResolutionFactor: 1 = 320x240, 2 = 640x480, 3 = 960x720 (fills a 720p screen's height).
        int resolution_factor = 2;
        // Picture shape on the 16:9 screen (GLideN64 AspectRatio):
        //   0 = 4:3 like the original (black bars left and right),
        //   1 = widescreen: 3D scenes are widened to fill the screen, 2D screens stay 4:3 in the middle (GLideN64 "adjust").
        //       Experimental: Stadium's menus and arena transitions show transparent bars and glitches in this mode,
        //   2 = stretch everything to 16:9 (no bars anywhere, 4:3 art is widened by a third).
        int widescreen = 2;
        // SDL audio callback size in frames. SDL's Switch backend releases one buffer per ~10 ms service tick, so
        // buffers under 1024 frames starved the DK64 port (25.6k of 48k frames/s).
        int audio_buffer_frames = 1024;
        // Nintendo layout: the game's A (confirm) is the Switch A button on the right, its B (cancel) the Switch B button
        // below. SDL's A/B are positional (A below, B right), so 0 gives the old positional mapping.
        bool nintendo_buttons = true;
        // Record every call the static recompile has no function for (coverage/runtime_captures.json in the data folder).
        bool interp_discovery = true;
        // While the game computes without drawing (a loading stall), ask the OS for its CPU boost mode (1785 MHz).
        bool stall_boost = true;
        // Left stick also presses the N64 D-pad, so it navigates Stadium's menus (which read only the D-pad). The analog
        // stick value is still passed through for the games that use it.
        bool left_stick_dpad = true;
        // Game Boy cartridge picker at start-up: 0 never, 1 auto (first run with ROMs on the card, or L held), 2 always.
        int gb_picker = 1;
        // How long (ms) the start-up prompt waits for L/ZL/R/ZR/- to open the picker once launcher.cfg exists; 0 = no prompt (then the
        // button must already be held when the program starts).
        int picker_wait_ms = 1500;
        // GLideN64 experiments for the menu jitter (-1 = leave GLideN64's own default):
        //   gl_copy_color: EnableCopyColorToRDRAM 0 = off, 1 = sync, 2 = double buffer (GLideN64's default here), 3 = triple buffer.
        //   gl_swap_mode:  BufferSwapMode 0 = every VI, 1 = when the VI origin changes, 2 = when the colour image changes.
        // Default 0: the colour-buffer readback to RDRAM cost hundreds of ms on the big page-transition display lists (A/B on device).
        int gl_copy_color = 0;
        int gl_swap_mode = -1;
        // After a video-mode change, pictures GLideN64 can only take straight from RDRAM (no rendered buffer yet) are garbage; the previous
        // picture stays up for at most this many presents instead (0 = show them). The hold ends at the first rendered picture. 150 (2.5 s):
        // the pick-Pokemon -> pick-team change leaves ~100 presents (1.7 s) with no display list at all (build 46), 30 only hid the first half.
        int vi_hold = 150;
        // 1 = keep the Transfer Pak access log (gbcart_ring.txt, written whenever the pak goes quiet for a second). OFF by default: while
        // the file is written (about 1 ms per entry, up to 11 s) the writer holds the pak's mutex, so the game's next Transfer Pak access
        // waited for it - the knock-out freezes of builds 42-46 (4.4 s, 7.7 s, 11 s: one entry per ~0.93 ms of freeze).
        int gbcart_ring = 0;
        // 1 = the two log files are written through big memory buffers that a background thread empties every 50 ms. 0 = every line is
        // its own write to the SD card. Each Horizon write costs ~4 ms (libnx flushes after every write), and the runtime prints ~290
        // lines in one go whenever the game loads an overlay it does not know (every knock-out: the candidate list of the "hash miss"):
        // ~1.1 s with the game stopped (build 47). Cost of 1: the last <=50 ms of log can be missing after a hard crash.
        int log_buffered = 1;
        // 1 = let the runtime write hash_miss_id_EF.bin (50 KB, one write per ~1 KB = ~0.3 s) at every such miss; 0 = off (default): the
        // name is taken by a folder, so fopen fails and the dump is skipped. The dump is only of use for teaching the generator the variant.
        int hash_miss_dump = 0;
        // GB Tower screen: 0 = as the game draws it (bezel off to the left), 1 = crop to the bezel, Game Boy proportions, centred,
        // 2 = crop and stretch over the whole screen, 3 = crop into a centred 4:3 box.
        int gb_view = 2;
        // 1 = present probe: read every presented frame back, log how far it moved against the previous one (vi_probe.txt) and
        // save a few consecutive frames as vi_frame_*.pgm. Slows the game down; for diagnosis only.
        bool vi_probe = false;
        // Which field Stadium's 480i menus are shown as: 1 = always the upper field (default; steady picture), 2 = always the lower,
        // 0 = GLideN64's own alternation (the picture moves one line up and down every frame).
        int vi_field = 1;
        // Start-up shader warm-up: before the game starts, build the GLideN64 combiner shaders earlier runs needed (a built-in list plus
        // shader_keys.txt, which grows by itself), for at most this many ms. Mesa on the Switch takes ~260 ms per shader and has no
        // cache, so each effect drawn for the first time froze the game (Gym Leader Castle). 0 = off (shaders then build on demand).
        int shader_warm_ms = 45000;
        // After the shaders the game really built, also build the ones a scan of the ROM and the decomp says it may draw with
        // (switch/shader_candidates.h): 0 = no, 1 = the likely ones (about 110, +18 s start-up), 2 = all (about 440, +70 s, needs a larger
        // shader_warm_ms). The scan found 41 of the 45 distinct effects a Gym Leader Castle run needed.
        int shader_warm_extra = 1;
        // Start-up time. The game records every combiner key it draws with in shader_used.txt (only ever grows). 1 (default): once 40 are
        // recorded the warm-up builds only those (+ shader_keys.txt), no built-in list and no ROM-scan guesses: the start-up shrinks to
        // what you really play, a never-seen effect builds once on demand (~150-500 ms) and is then remembered. 2: the same but the
        // built-in list stays. 0: off, always the full list (about 135 shaders, ~24 s).
        int shader_prune = 1;
        // Mesa's nouveau back end optimises every shader it translates (NV50_PROG_OPTIMIZE, default 3). A lower level compiles faster
        // and produces somewhat slower shader code. 0-3 = set that level, -1 = leave Mesa's default. Default 1.
        int shader_opt = 1;
        // 1 = while the shaders are built at start-up (20+ s with nothing on the screen), show a "Please wait" picture in the style of the game's
        // menus (drawn from the player's ROM) with a progress bar. 0 = leave the window black until the game starts.
        int splash = 1;
    };

    const Config& config();
    int screen_width();
    int screen_height();
}

// Bring-up diagnostics: counters the heartbeat thread in main_switch.cpp prints every few seconds.
namespace psrnx_diag {
    extern std::atomic<uint64_t> vi_ticks;        // VI callbacks (the VI thread is alive and the game is running)
    extern std::atomic<uint64_t> send_dl;         // display lists handed to GLideN64
    extern std::atomic<uint64_t> update_screen;   // present calls
    extern std::atomic<uint64_t> audio_pushes;    // audio buffers the game queued
    extern std::atomic<uint64_t> audio_frames;    // audio frames the game queued
    extern std::atomic<uint64_t> send_dl_ns;      // total time spent inside GLideN64 ProcessDList
    extern std::atomic<uint64_t> update_screen_ns;// total time spent inside GLideN64 UpdateScreen
    extern std::atomic<bool> in_process_dl;       // the gfx thread is inside GLideN64's ProcessDList
    extern std::atomic<uint64_t> slow_calls;      // calls over the slow threshold (logged individually, first 80)
}
