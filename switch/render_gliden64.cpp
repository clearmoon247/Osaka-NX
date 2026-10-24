// GLideN64 behind ultramodern's RendererContext, replacing src/main/rt64_render_context.cpp on Switch.
//
// The mechanism is DK64Recomp-NX's (which runs on the same console): GLideN64's mupen64plus GFX_INFO is pointed at
// the recomp's RDRAM, whose 32-bit words are already in the host order GLideN64 expects (RT64 is fed the same way
// on PC). What is Stadium-specific here:
//   - the ROM header name GLideN64 keys its per-game settings on ("POKEMON STADIUM"),
//   - RDRAM_SIZE reported as 8 MiB (Stadium uses the Expansion Pak path, see stadium_setup.cpp; the 1 GiB guest
//     window is a runtime artifact and must not reach GLideN64's memory-size heuristics),
//   - none of DK64's 30->60 fps in-between-frame machinery. Stadium renders at 60 VI/s natively.
// The CPU/RSP-written GB Tower framebuffers are shown through GLideN64's RDRAM-to-colour-buffer path plus the present hook below.
#include <cstdio>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <mutex>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

#include <SDL.h>
#include <switch.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_common.h"
#include "m64p_plugin.h"

#include "psrnx.h"
#include "shader_candidates.h"
#include "gbpicker_gfx.h"
#include "stadium_skin.h"
#include "ultramodern/renderer_context.hpp"

void psrnx_seed_gliden64_config();
extern "C" int psrnx_vi_field_mode;                  // FrameBuffer.cpp (patch gliden64-vifield.patch)
extern "C" int psrnx_skip_swap;  // core_switch.cpp
extern "C" void (*psrnx_present_hook)(const int* v);  // FrameBuffer.cpp (patch gliden64-viprobe.patch)
extern "C" unsigned long long psrnx_ms_since_boot();
extern "C" int psrnx_shader_warm_ms;                  // Combiner.cpp (patch gliden64-shaderwarm.patch)

// Shader warm-up list (see Combiner.cpp). GLideN64 builds one GL program per combiner key (the 64-bit mux plus mode bits) and Mesa takes
// ~260 ms for each, on the GL thread, the first time an effect is drawn. The keys below are the 47 Gym Leader Castle runs needed (build 41 and 44
// logs), in the order they were needed; shader_keys.txt next to osaka-nx.cfg collects every other key the game ever had to build on demand
// (one 16-digit hex number per line), so the list grows with what you play.
namespace {
    constexpr unsigned long long SHADER_SEED[] = {
        0x3DFFFFFFFFFCF279ULL, 0x3FFFFFFFFFFE793CULL, 0x38FFFFFFFFFEFB7DULL, 0x3A11FE04FF1BF3FFULL, 0x39FFFFFFFFFDF6FBULL,
        0x39FFFFFFFFFCF87CULL, 0x39129A25FF37FFFFULL, 0x39509AA13335FEFFULL, 0x3AFFFFFFFFFCF238ULL, 0x38121824FF33FFFFULL,
        0x38567EAC44FE793CULL, 0x3A566BFF4F12123FULL, 0x39309A615536FF7FULL, 0x3B517E023F55FFFFULL, 0x3AFF9BFFFFFCFE38ULL,
        0x3AFF9BFFFFFFFE38ULL, 0x3A11FE04FF1BF7FFULL, 0x3AFFFE04FF19F7FFULL, 0x3A1197FFFF1BFE3FULL, 0x3AFFFFFFFF18F23FULL,
        0x3A309BFF5F1AFE3FULL, 0x3A121DFFFFFFFE38ULL, 0x3AFFFFFFFFFCF838ULL, 0x3A127FFFFFFFF838ULL, 0x3A3097FF5FFEFE38ULL,
        0x3A119604FF1BFFFFULL, 0x3AFF9BFFFF18FE3FULL, 0x3BFFFFFFFFFDF638ULL, 0x39119623FF2FFFFFULL, 0x39FFFFFFFFFCF279ULL,
        0x3B12ABFFFFFFFE38ULL, 0x3A277E60150CF37FULL, 0x3A2726601514937FULL, 0x38309661552EFF7FULL, 0x3A127FFFFFFFF238ULL,
        0x38127E24FFFFF3F9ULL, 0x385098A144327F3FULL, 0x3A277E6015FCF778ULL, 0x3AFFFE03FF0F73FFULL, 0x38FFFFFFFFFE793CULL,
        // Found by the build 44 run (not in the build 41 list): the first KO and the pick-Pokemon -> pick-team transition.
        0x3A121E03FFFFFFF8ULL, 0x3A272C60150C937FULL, 0x3A30FE045FFEF3F8ULL, 0x3AFFFFFFFF18F83FULL, 0x3A129BFFFF1BFE3FULL,
        0x3A129BFFFFFFFE38ULL, 0x3A12FFFFFFFFF238ULL,
        // Found by the build 46 run (still built on demand there, 281 ms and 512 ms): a menu effect and the first battle frame.
        0x3A3096805F1AFFFFULL, 0x3A11FE25FFFFFC38ULL,
    };

    // Keys the game really drew with, on this console (shader_used.txt, one 16-digit hex number per line, only ever grows). Filled by the
    // GLideN64 patch gliden64-shaderused.patch (setCombine). Once it holds enough keys the start-up warm-up builds just these (shader_prune).
    constexpr size_t SHADER_USED_TRAINED = 40;
    std::mutex g_used_mutex;
    std::unordered_set<unsigned long long> g_used_all;     // file + this run
    std::vector<unsigned long long> g_used_pending;        // not yet appended to shader_used.txt
    std::vector<unsigned long long> g_used_file_order;     // as read from the file
    unsigned g_used_new_this_run = 0;
    bool g_used_loaded = false, g_used_writer_started = false;

    void used_writer_loop() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            std::vector<unsigned long long> out;
            size_t total = 0;
            unsigned fresh = 0;
            {
                std::lock_guard<std::mutex> lock(g_used_mutex);
                out.swap(g_used_pending);
                total = g_used_all.size();
                fresh = g_used_new_this_run;
            }
            if (out.empty()) continue;
            if (FILE* f = std::fopen(PSRNX_DATA_DIR "/shader_used.txt", "a")) {   // off the GL thread: an SD write is ~5 ms
                for (unsigned long long key : out) std::fprintf(f, "%016llX\n", key);
                std::fclose(f);
            }
            std::printf("[PSRNX] shader usage: %zu new keys saved to shader_used.txt (%zu recorded, %u new this run)\n", out.size(), total, fresh);
        }
    }

    void used_load() {
        std::lock_guard<std::mutex> lock(g_used_mutex);
        if (g_used_loaded) return;
        g_used_loaded = true;
        if (FILE* f = std::fopen(PSRNX_DATA_DIR "/shader_used.txt", "r")) {
            char line[128];
            while (std::fgets(line, sizeof(line), f)) {
                char* end = nullptr;
                const unsigned long long key = std::strtoull(line, &end, 16);
                if (end == line || key == 0 || (end - line) != 16) continue;   // a half-written last line
                if (g_used_all.insert(key).second) g_used_file_order.push_back(key);
            }
            std::fclose(f);
        }
    }

    // How long every combiner build took (patch hook in Combiner_Compile): at the end of a burst, a summary and shader_warm.txt.
    struct CompileRec { unsigned long long mux; long us; };
    std::mutex g_comp_mutex;
    std::vector<CompileRec> g_comp;
    size_t g_comp_reported = 0;
    unsigned long long g_comp_last_ms = 0;
    bool g_comp_thread_started = false;

    void compile_report_loop() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::vector<CompileRec> all;
            size_t from = 0;
            {
                std::lock_guard<std::mutex> lock(g_comp_mutex);
                if (g_comp.size() == g_comp_reported || psrnx_ms_since_boot() < g_comp_last_ms + 1500) continue;
                all = g_comp;
                from = g_comp_reported;
                g_comp_reported = g_comp.size();
            }
            std::vector<long> ms;
            long total = 0;
            for (size_t i = from; i < all.size(); ++i) { ms.push_back(all[i].us / 1000); total += all[i].us / 1000; }
            std::sort(ms.begin(), ms.end());
            if (ms.size() >= 5) {
                std::printf("[PSRNX] shader build times: %zu builds, %ld ms in all, mean %ld, median %ld, 90%% under %ld, slowest %ld ms (shader_warm.txt)\n",
                            ms.size(), total, total / static_cast<long>(ms.size()), ms[ms.size() / 2], ms[ms.size() * 9 / 10], ms.back());
            }
            std::vector<CompileRec> sorted = all;
            std::sort(sorted.begin(), sorted.end(), [](const CompileRec& a, const CompileRec& b) { return a.us > b.us; });
            if (FILE* f = std::fopen(PSRNX_DATA_DIR "/shader_warm.txt", "w")) {
                std::fprintf(f, "# every combiner build of this run, slowest first: milliseconds, key (%zu builds)\n", sorted.size());
                for (const CompileRec& r : sorted) std::fprintf(f, "%ld %016llX\n", r.us / 1000, r.mux);
                std::fclose(f);
            }
        }
    }

    std::mutex g_shader_keys_mutex;
    std::vector<unsigned long long> g_shader_keys;   // seed, shader_keys.txt, keys added this run, then the static candidates (order = build order)
    std::set<unsigned long long> g_shader_key_set;   // everything in g_shader_keys
    std::set<unsigned long long> g_shader_learned;   // seed + shader_keys.txt + this run: keys the game really built (the candidates are not)
    bool g_shader_keys_loaded = false;

    void shader_keys_load_locked() {
        if (g_shader_keys_loaded) return;
        g_shader_keys_loaded = true;
        used_load();
        // Trained: shader_used.txt knows enough keys. Then the warm-up builds what the game really drew with on this console (plus
        // shader_keys.txt) and nothing else: no built-in list (shader_prune=2 keeps it), no guesses from the ROM scan.
        const int prune = psrnx::config().shader_prune;
        const bool trained = prune != 0 && g_used_file_order.size() >= SHADER_USED_TRAINED;
        for (unsigned long long key : SHADER_SEED) {
            if (!trained || prune == 2) {
                if (g_shader_key_set.insert(key).second) g_shader_keys.push_back(key);
            }
            g_shader_learned.insert(key);
        }
        if (trained) {
            for (unsigned long long key : g_used_file_order) {
                if (g_shader_key_set.insert(key).second) g_shader_keys.push_back(key);
            }
        }
        unsigned from_file = 0;
        if (FILE* f = std::fopen(PSRNX_DATA_DIR "/shader_keys.txt", "r")) {
            char line[128];
            while (std::fgets(line, sizeof(line), f)) {
                if (line[0] == '#') continue;
                char* end = nullptr;
                const unsigned long long key = std::strtoull(line, &end, 16);
                if (end == line || key == 0) continue;
                g_shader_learned.insert(key);
                if (g_shader_key_set.insert(key).second) {
                    g_shader_keys.push_back(key);
                    ++from_file;
                }
            }
            std::fclose(f);
        }
        // Keys the game may draw with, from a scan of the ROM and the decomp (shader_candidates.h), after everything it really built.
        // The scan can contain a word the shader builder chokes on. A start that dies inside the warm-up leaves shader_warm.lock behind
        // (so does leaving the game while it boots). First time: the next start skips the extras once (the lock becomes
        // shader_warm_crashed.txt). If the following start with extras dies as well, they are off for good (shader_warm_off.txt;
        // delete it to try again).
        int extra = trained ? 0 : psrnx::config().shader_warm_extra;
        if (!trained) {
            struct stat st;
            const bool lock = stat(PSRNX_DATA_DIR "/shader_warm.lock", &st) == 0;
            const bool crashed = stat(PSRNX_DATA_DIR "/shader_warm_crashed.txt", &st) == 0;
            if (lock && crashed) {
                std::rename(PSRNX_DATA_DIR "/shader_warm.lock", PSRNX_DATA_DIR "/shader_warm_off.txt");
                std::printf("[PSRNX] shader keys: two starts in a row did not finish the extra-shader warm-up; extras are off until shader_warm_off.txt is deleted\n");
            } else if (lock) {
                std::rename(PSRNX_DATA_DIR "/shader_warm.lock", PSRNX_DATA_DIR "/shader_warm_crashed.txt");
                std::printf("[PSRNX] shader keys: the previous start did not finish the shader warm-up; extras are skipped on this start\n");
                extra = 0;
            }
            if (stat(PSRNX_DATA_DIR "/shader_warm_off.txt", &st) == 0) extra = 0;
            if (extra > 0) {
                if (FILE* f = std::fopen(PSRNX_DATA_DIR "/shader_warm.lock", "w")) {
                    std::fputs("a start-up did not get past building the extra shaders\n", f);
                    std::fclose(f);
                }
            }
        }
        unsigned likely = 0, rare = 0;
        if (extra >= 1) {
            for (unsigned long long key : SHADER_LIKELY) {
                if (g_shader_key_set.insert(key).second) { g_shader_keys.push_back(key); ++likely; }
            }
        }
        if (extra >= 2) {
            for (unsigned long long key : SHADER_RARE) {
                if (g_shader_key_set.insert(key).second) { g_shader_keys.push_back(key); ++rare; }
            }
        }
        std::printf("[PSRNX] shader keys: %zu in the list (%zu built in, %u more from shader_keys.txt, %u likely + %u rare from the ROM scan; "
                    "shader_warm_extra=%d)\n", g_shader_keys.size(), sizeof(SHADER_SEED) / sizeof(SHADER_SEED[0]), from_file, likely, rare, extra);
        std::printf("[PSRNX] shader usage: %zu keys recorded in shader_used.txt; shader_prune=%d -> %s\n", g_used_file_order.size(), prune,
                    trained ? (prune == 2 ? "trained: recorded keys + the built-in list are built up front"
                                              : "trained: only recorded keys (and shader_keys.txt) are built up front")
                            : (prune == 0 ? "off: full list" : "not trained yet (needs 40 keys, they are recorded while you play): full list this start"));
    }
}

// Start-up splash. The shader warm-up (CombinerInfo::init, patch gliden64-shaderwarm) runs on the GL thread before the game draws anything and
// takes 20+ s, during which the window would stay black. While it runs, a Game Pak Check-style picture ("Please wait", the menu background and
// font read from the player's own ROM, a progress bar) is drawn through GL: rendered on the CPU by gbpicker::Gfx into 640x480, uploaded as a
// texture and stretched into the window with glBlitFramebuffer. Every GL state it touches is saved and put back, GLideN64 caches state.
// It is shown from psrnx_shader_keys (the call right before the build loop) and refreshed from psrnx_shader_compiled (after every build),
// at most every 500 ms; the constructor of the renderer context ends it. osaka-nx.cfg `splash=0` turns it off.
extern SDL_Window* psrnx_window;   // main_switch.cpp

namespace {
    struct SplashGl {
        using GenFn = void (*)(int, unsigned*);
        using BindFn = void (*)(unsigned, unsigned);
        using GetIntFn = void (*)(unsigned, int*);
        using EnableFn = void (*)(unsigned);
        void (*delete_tex)(int, const unsigned*) = nullptr;
        void (*delete_fb)(int, const unsigned*) = nullptr;
        void (*tex_image)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*) = nullptr;
        void (*tex_sub_image)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*) = nullptr;
        void (*tex_param)(unsigned, unsigned, int) = nullptr;
        void (*fb_tex)(unsigned, unsigned, unsigned, unsigned, int) = nullptr;
        unsigned (*fb_status)(unsigned) = nullptr;
        void (*blit)(int, int, int, int, int, int, int, int, unsigned, unsigned) = nullptr;
        void (*clear_color)(float, float, float, float) = nullptr;
        void (*clear)(unsigned) = nullptr;
        void (*color_mask)(unsigned char, unsigned char, unsigned char, unsigned char) = nullptr;
        void (*get_float)(unsigned, float*) = nullptr;
        void (*get_bool)(unsigned, unsigned char*) = nullptr;
        unsigned char (*is_enabled)(unsigned) = nullptr;
        void (*pixel_store)(unsigned, int) = nullptr;
        unsigned (*get_error)() = nullptr;
        GenFn gen_tex = nullptr, gen_fb = nullptr;
        BindFn bind_tex = nullptr, bind_fb = nullptr, bind_buf = nullptr;
        GetIntFn get_int = nullptr;
        EnableFn enable = nullptr, disable = nullptr;
    };

    struct Splash {
        std::atomic<bool> active{false};   // set around the renderer constructor
        bool failed = false;               // no skin, no GL entry points, framebuffer incomplete: nothing is drawn
        bool shown = false;                // at least one frame is on screen
        std::thread loader;                // reads the skin from the ROM while GLideN64 starts up
        skin::Skin skin;
        std::unique_ptr<gbpicker::Gfx> gfx;
        std::vector<uint32_t> flipped;     // the canvas, bottom row first (GL's texture order)
        SplashGl gl;
        unsigned tex = 0, fbo = 0;
        unsigned total = 0, done = 0;
        std::chrono::steady_clock::time_point last{};
        std::chrono::steady_clock::time_point first{};
    } g_splash;

    template <class F> F splash_proc(const char* name) { return reinterpret_cast<F>(SDL_GL_GetProcAddress(name)); }

    bool splash_gl_load() {
        SplashGl& g = g_splash.gl;
        bool ok = true;
#define PSRNX_SP(member, name) g.member = splash_proc<decltype(g.member)>(name); if (g.member == nullptr) ok = false;
        PSRNX_SP(delete_tex, "glDeleteTextures") PSRNX_SP(delete_fb, "glDeleteFramebuffers") PSRNX_SP(tex_image, "glTexImage2D")
        PSRNX_SP(tex_sub_image, "glTexSubImage2D") PSRNX_SP(tex_param, "glTexParameteri") PSRNX_SP(fb_tex, "glFramebufferTexture2D")
        PSRNX_SP(fb_status, "glCheckFramebufferStatus") PSRNX_SP(blit, "glBlitFramebuffer") PSRNX_SP(clear_color, "glClearColor")
        PSRNX_SP(clear, "glClear") PSRNX_SP(color_mask, "glColorMask") PSRNX_SP(get_float, "glGetFloatv") PSRNX_SP(get_bool, "glGetBooleanv")
        PSRNX_SP(is_enabled, "glIsEnabled") PSRNX_SP(pixel_store, "glPixelStorei") PSRNX_SP(get_error, "glGetError") PSRNX_SP(gen_tex, "glGenTextures")
        PSRNX_SP(gen_fb, "glGenFramebuffers") PSRNX_SP(bind_tex, "glBindTexture") PSRNX_SP(bind_fb, "glBindFramebuffer") PSRNX_SP(bind_buf, "glBindBuffer")
        PSRNX_SP(get_int, "glGetIntegerv") PSRNX_SP(enable, "glEnable") PSRNX_SP(disable, "glDisable")
#undef PSRNX_SP
        return ok;
    }

    // Before the renderer starts: begin reading the skin (font, panels, background) from the ROM in the data folder.
    void splash_begin() {
        Splash& s = g_splash;
        if (psrnx::config().splash == 0 || psrnx::config().shader_warm_ms <= 0) return;
        s.active = true;
        s.loader = std::thread([]() {
            Splash& sp = g_splash;
            sp.skin = skin::Skin{};
            if (!skin::load_from_folder(PSRNX_DATA_DIR, sp.skin)) { std::printf("[PSRNX] splash: no picture (%s)\n", sp.skin.error.c_str()); sp.failed = true; }
        });
    }

    // One frame: progress < 0 = no bar.
    void splash_draw(const char* head1, const char* head2, const std::vector<std::string>& lines, float progress, const std::string& caption) {
        Splash& s = g_splash;
        SplashGl& g = s.gl;
        s.gfx->splash(head1, head2, lines, progress, caption);
        constexpr int W = gbpicker::Gfx::kW, H = gbpicker::Gfx::kH;
        const std::vector<uint32_t>& canvas = s.gfx->canvas();
        for (int y = 0; y < H; ++y)
            std::memcpy(&s.flipped[static_cast<size_t>(H - 1 - y) * W], &canvas[static_cast<size_t>(y) * W], static_cast<size_t>(W) * 4);

        int win_w = 0, win_h = 0;
        SDL_GL_GetDrawableSize(psrnx_window, &win_w, &win_h);
        if (win_w <= 0 || win_h <= 0) return;
        int x0 = 0, y0 = 0, x1 = win_w, y1 = win_h;
        const bool stretch = psrnx::config().widescreen == 2;
        if (!stretch) {
            const int w = std::min(win_w, win_h * W / H);
            x0 = (win_w - w) / 2; x1 = x0 + w;
        }

        // Save.
        int old_draw_fb = 0, old_read_fb = 0, old_tex = 0, old_unpack_buf = 0, old_row = 0, old_align = 4;
        float old_clear[4] = {0, 0, 0, 0};
        unsigned char old_mask[4] = {1, 1, 1, 1};
        g.get_int(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &old_draw_fb);
        g.get_int(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &old_read_fb);
        g.get_int(0x8069 /* TEXTURE_BINDING_2D */, &old_tex);
        g.get_int(0x88EF /* PIXEL_UNPACK_BUFFER_BINDING */, &old_unpack_buf);
        g.get_int(0x0CF2 /* UNPACK_ROW_LENGTH */, &old_row);
        g.get_int(0x0CF5 /* UNPACK_ALIGNMENT */, &old_align);
        g.get_float(0x0C22 /* COLOR_CLEAR_VALUE */, old_clear);
        g.get_bool(0x0C23 /* COLOR_WRITEMASK */, old_mask);
        const bool scissor = g.is_enabled(0x0C11 /* SCISSOR_TEST */) != 0;

        if (g.get_error != nullptr) while (g.get_error() != 0) {}   // not ours to report
        bool bad = false;
        if (s.tex == 0) {
            g.gen_tex(1, &s.tex);
            g.gen_fb(1, &s.fbo);
            g.bind_tex(0x0DE1, s.tex);
            g.tex_param(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2601 /* LINEAR */);
            g.tex_param(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2601);
            g.tex_param(0x0DE1, 0x2802 /* WRAP_S */, 0x812F /* CLAMP_TO_EDGE */);
            g.tex_param(0x0DE1, 0x2803 /* WRAP_T */, 0x812F);
        } else {
            g.bind_tex(0x0DE1, s.tex);
        }
        if (g.bind_buf != nullptr && old_unpack_buf != 0) g.bind_buf(0x88EC /* PIXEL_UNPACK_BUFFER */, 0);
        g.pixel_store(0x0CF2, 0);
        g.pixel_store(0x0CF5, 4);
        if (!s.shown) g.tex_image(0x0DE1, 0, 0x8058 /* RGBA8 */, W, H, 0, 0x1908 /* RGBA */, 0x1401 /* UNSIGNED_BYTE */, s.flipped.data());
        else g.tex_sub_image(0x0DE1, 0, 0, 0, W, H, 0x1908, 0x1401, s.flipped.data());
        g.bind_fb(0x8CA8 /* READ_FRAMEBUFFER */, s.fbo);
        g.fb_tex(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */, 0x0DE1, s.tex, 0);
        if (g.fb_status(0x8CA8) != 0x8CD5 /* COMPLETE */) {
            std::printf("[PSRNX] splash: read buffer incomplete, no picture\n");
            s.failed = true;
            bad = true;
        } else {
            g.bind_fb(0x8CA9 /* DRAW_FRAMEBUFFER */, 0);
            if (scissor) g.disable(0x0C11);
            g.color_mask(1, 1, 1, 1);
            if (x0 > 0) { g.clear_color(0, 0, 0, 1); g.clear(0x4000 /* COLOR_BUFFER_BIT */); }
            g.blit(0, 0, W, H, x0, y0, x1, y1, 0x4000, 0x2601 /* LINEAR */);
        }

        // Restore.
        g.color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
        g.clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
        if (scissor) g.enable(0x0C11);
        g.bind_fb(0x8CA9, static_cast<unsigned>(old_draw_fb));
        g.bind_fb(0x8CA8, static_cast<unsigned>(old_read_fb));
        g.bind_tex(0x0DE1, static_cast<unsigned>(old_tex));
        g.pixel_store(0x0CF2, old_row);
        g.pixel_store(0x0CF5, old_align);
        if (g.bind_buf != nullptr && old_unpack_buf != 0) g.bind_buf(0x88EC, static_cast<unsigned>(old_unpack_buf));
        if (g.get_error != nullptr) while (g.get_error() != 0) {}
        if (bad) return;
        s.shown = true;
        SDL_GL_SwapWindow(psrnx_window);
    }

    std::vector<std::string> splash_lines(bool starting) {
        if (starting) return {"", "Starting the game...", ""};
        return {"Preparing the graphics.", "The game starts when the bar is full.", "This only happens at start-up."};
    }

    // Right before the build loop: the skin must be there, then the first picture.
    void splash_start(unsigned keys) {
        Splash& s = g_splash;
        if (!s.active || s.failed || s.gfx) return;
        if (s.loader.joinable()) s.loader.join();
        if (s.failed) return;
        if (!splash_gl_load() || psrnx_window == nullptr) { std::printf("[PSRNX] splash: GL entry points missing, no picture\n"); s.failed = true; return; }
        s.gfx = std::make_unique<gbpicker::Gfx>(s.skin);
        s.flipped.assign(static_cast<size_t>(gbpicker::Gfx::kW) * gbpicker::Gfx::kH, 0);
        s.total = keys;
        s.first = s.last = std::chrono::steady_clock::now();
        splash_draw("Please", "Wait", splash_lines(false), 0.0f, "0 / " + std::to_string(keys));
        std::printf("[PSRNX] splash: shown (%u shaders to build)\n", keys);
    }

    // After each build.
    void splash_progress() {
        Splash& s = g_splash;
        if (!s.active || s.failed || !s.gfx) return;
        ++s.done;
        const auto now = std::chrono::steady_clock::now();
        if (now - s.last < std::chrono::milliseconds(500)) return;
        s.last = now;
        const unsigned n = std::min(s.done, s.total);
        splash_draw("Please", "Wait", splash_lines(false), s.total ? static_cast<float>(n) / static_cast<float>(s.total) : 0.0f,
                    std::to_string(n) + " / " + std::to_string(s.total));
    }

    // The renderer is up (or failed to come up): last picture, then drop everything.
    void splash_end() {
        Splash& s = g_splash;
        if (!s.active) return;
        s.active = false;
        if (s.loader.joinable()) s.loader.join();
        if (s.gfx && !s.failed) splash_draw("Please", "Wait", splash_lines(true), 1.0f, "");
        if (s.gfx) {
            if (s.tex != 0 && s.gl.delete_tex) s.gl.delete_tex(1, &s.tex);
            if (s.fbo != 0 && s.gl.delete_fb) s.gl.delete_fb(1, &s.fbo);
            std::printf("[PSRNX] splash: shown for %lld ms\n",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s.first).count()));
            s.tex = s.fbo = 0;
            s.gfx.reset();
            s.skin = skin::Skin{};
            s.flipped = std::vector<uint32_t>();
        }
    }

    struct SplashGuard {
        SplashGuard() { splash_begin(); }
        ~SplashGuard() { splash_end(); }
    };
}

extern "C" unsigned psrnx_shader_keys(unsigned long long* out, unsigned max) {
    std::lock_guard<std::mutex> lock(g_shader_keys_mutex);
    shader_keys_load_locked();
    unsigned n = 0;
    for (unsigned long long key : g_shader_keys) {
        if (n >= max) break;
        out[n++] = key;
    }
    splash_start(n);
    return n;
}

// A shader had to be built while the game ran (not built up front): remember it, the next start builds it early.
extern "C" void psrnx_shader_key_used(unsigned long long mux) {
    static unsigned long long last = 0;   // the GL thread is the only caller
    if (mux == last) return;
    last = mux;
    std::lock_guard<std::mutex> lock(g_used_mutex);
    if (!g_used_loaded) return;           // not before the warm-up has read the file
    if (!g_used_all.insert(mux).second) return;
    g_used_pending.push_back(mux);
    ++g_used_new_this_run;
    if (!g_used_writer_started) { g_used_writer_started = true; std::thread(used_writer_loop).detach(); }
}

extern "C" void psrnx_shader_compiled(unsigned long long mux, long us) {
    {
        std::lock_guard<std::mutex> lock(g_comp_mutex);
        g_comp.push_back({mux, us});
        g_comp_last_ms = psrnx_ms_since_boot();
        if (!g_comp_thread_started) { g_comp_thread_started = true; std::thread(compile_report_loop).detach(); }
    }
    splash_progress();   // start-up splash (a no-op once the renderer is up)
}

extern "C" void psrnx_shader_key_seen(unsigned long long mux) {
    psrnx_shader_key_used(mux);
    std::lock_guard<std::mutex> lock(g_shader_keys_mutex);
    shader_keys_load_locked();
    if (!g_shader_learned.insert(mux).second) return;
    if (g_shader_key_set.insert(mux).second) g_shader_keys.push_back(mux);
    if (FILE* f = std::fopen(PSRNX_DATA_DIR "/shader_keys.txt", "a")) {
        std::fprintf(f, "%016llX\n", mux);
        std::fclose(f);
    }
    std::printf("[PSRNX] shader keys: new key %016llX saved to shader_keys.txt (%zu learned)\n", mux, g_shader_learned.size());
}

namespace {
    // OSTask is copied where the RSP boot code leaves it, at the end of DMEM.
    constexpr uint32_t TASK_DMEM_OFFSET = 0xFC0;
    constexpr uint32_t KSEG_MASK = 0x1FFFFFFF;
    // libultra's ROM header: GLideN64 reads the internal name (20 bytes at 0x20) for its per-game tweaks.
    constexpr char ROM_NAME[] = "POKEMON STADIUM    ";

    uint8_t dmem[0x1000];
    uint8_t imem[0x1000];
    uint8_t rom_header[0x40];
    unsigned int mi_intr_reg;
    unsigned int sp_status_reg;
    unsigned int dpc_start_reg, dpc_end_reg, dpc_current_reg, dpc_status_reg;
    unsigned int dpc_clock_reg, dpc_bufbusy_reg, dpc_pipebusy_reg, dpc_tmem_reg;
    const unsigned int rdram_size = 8 * 1024 * 1024;

    // Video-mode diagnostics: the VI registers at every presented frame plus how many display lists ran since the last one.
    // Written to vi_ring.txt every 5 s (overwritten), distinct register combinations are also logged when first seen.
    struct ViSample {
        uint64_t n;
        uint32_t ms;
        uint32_t dls;     // display lists run since the previous present
        uint32_t status, origin, width, vstart, hstart, yscale, xscale, vsync;
        // Filled by the present hook (FrameBuffer.cpp): how GLideN64 composed this present, see vi_dump for the order.
        uint32_t ms_end;
        int32_t has_pv;
        int32_t pv[32];
        // Probe results (vi_probe=1): picture hash, vertical shift against the previous presented picture, error at no shift / at best shift.
        uint64_t hash;
        int32_t dy;
        uint32_t sad0, sadbest;
    };
    constexpr uint32_t VI_RING = 512;
    ViSample vi_ring[VI_RING];
    std::atomic<uint64_t> vi_samples{0};
    std::atomic<uint32_t> vi_dls_since_present{0};
    uint32_t vi_seen[64][7];
    uint32_t vi_seen_n = 0;

    // Total size of the files under a directory (shader caches), -1 if it does not exist.
    long long dir_bytes(const std::string& path, int* files) {
        DIR* d = opendir(path.c_str());
        if (d == nullptr) return -1;
        long long total = 0;
        while (const dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            const std::string child = path + "/" + e->d_name;
            struct stat st;
            if (stat(child.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                const long long sub = dir_bytes(child, files);
                if (sub > 0) total += sub;
            } else {
                total += st.st_size;
                ++*files;
            }
        }
        closedir(d);
        return total;
    }

    void shader_info_dump() {
        FILE* f = std::fopen(PSRNX_DATA_DIR "/shader_info.txt", "w");
        if (f == nullptr) return;
        int n1 = 0, n2 = 0;
        const long long b1 = dir_bytes(PSRNX_DATA_DIR "/shaders", &n1);
        const long long b2 = dir_bytes(PSRNX_DATA_DIR "/mesa_cache", &n2);
        std::fprintf(f, "t=%llu ms\nGLideN64 shader storage (shaders/): %d files, %lld bytes\nMesa shader cache (mesa_cache/): %d files, %lld bytes\n",
                     psrnx_ms_since_boot(), n1, b1, n2, b2);
        std::fclose(f);
    }

    // What the GL driver says about program binaries (GLideN64's shader storage needs at least one binary format).
    void log_gl_info() {
        using GetStringFn = const unsigned char* (*)(unsigned);
        using GetIntegervFn = void (*)(unsigned, int*);
        const auto get_string = reinterpret_cast<GetStringFn>(SDL_GL_GetProcAddress("glGetString"));
        const auto get_integerv = reinterpret_cast<GetIntegervFn>(SDL_GL_GetProcAddress("glGetIntegerv"));
        if (get_string == nullptr || get_integerv == nullptr) return;
        int formats = -1;
        get_integerv(0x87FE /* GL_NUM_PROGRAM_BINARY_FORMATS */, &formats);
        const auto str = [&](unsigned name) { const unsigned char* s = get_string(name); return s ? reinterpret_cast<const char*>(s) : "?"; };
        std::printf("[PSRNX] GL: renderer=%s version=%s vendor=%s program-binary-formats=%d\n", str(0x1F01), str(0x1F02), str(0x1F00), formats);
    }

    void vi_write_header(FILE* f, uint64_t shown, uint64_t total) {
        std::fprintf(f, "# %llu presents of %llu; columns: n  ms  dls  status origin width vstart hstart yscale xscale vsync"
                        " | ms_end has_pv | fb_origin fb_start fb_w fb_h scale*1000 srcY0 dstY0 lower addrOff src[4] dst[4] vres hres srcH next"
                        " texW texH xoffL xoffR winH winW viH viW inter vstart cfb | hash dy sad0 sadbest\n",
                     static_cast<unsigned long long>(shown), static_cast<unsigned long long>(total));
    }

    void vi_write_row(FILE* f, const ViSample& v) {
        std::fprintf(f, "%llu %u %u  %08X %08X %u %08X %08X %08X %08X %u | %u %d |", static_cast<unsigned long long>(v.n), v.ms, v.dls,
                     v.status, v.origin, v.width, v.vstart, v.hstart, v.yscale, v.xscale, v.vsync, v.ms_end, v.has_pv);
        for (int k = 0; k < 32; k++) {
            if (k < 2) std::fprintf(f, " %08X", static_cast<unsigned>(v.pv[k]));
            else       std::fprintf(f, " %d", v.pv[k]);
        }
        std::fprintf(f, " | %016llX %d %u %u\n", static_cast<unsigned long long>(v.hash), v.dy, v.sad0, v.sadbest);
    }

    // Mode-change capture: when status/width/scale change between two presents, keep the 20 presents before and 120 after in
    // vi_trans_<k>.txt (the 512-present ring is long gone by the time a session ends). At most 12 files.
    // Presents since the last video-mode change whose picture GLideN64 had to take straight from RDRAM (cfb=1) are not shown:
    // the game has not drawn into the new buffers yet, so it is whatever was in memory (the title -> Game Pak Check glitch).
    uint64_t last_mode_n = 0;
    bool good_shown = false;
    uint32_t hold_run = 0, hold_total = 0;

    struct TransState { bool active = false; uint64_t first = 0, last = 0; int files = 0; } trans;

    // The rows are copied here (on the present thread, microseconds) and written by a short-lived thread: writing the ~33 KB on the
    // present thread froze the picture for 120-210 ms, 2 s after every mode change (build 46: SLOW UpdateScreen 121 presents later).
    void trans_finish() {
        const std::string name = std::string(PSRNX_DATA_DIR "/vi_trans_") + std::to_string(trans.files) + ".txt";
        const uint64_t first = trans.first, last = trans.last;
        std::vector<ViSample> rows;
        rows.reserve(static_cast<size_t>(last - first + 1));
        for (uint64_t i = first; i <= last; i++) rows.push_back(vi_ring[i % VI_RING]);
        std::thread([name, first, last, rows = std::move(rows)]() {
            if (FILE* f = std::fopen(name.c_str(), "w")) {
                vi_write_header(f, last - first + 1, last + 1);
                for (const ViSample& r : rows) vi_write_row(f, r);
                std::fclose(f);
            }
        }).detach();
        trans.files++;
        trans.active = false;
    }

    void vi_dump() {
        const uint64_t total = vi_samples.load();
        if (total == 0) return;
        FILE* f = std::fopen(PSRNX_DATA_DIR "/vi_ring.txt", "w");
        if (f == nullptr) return;
        vi_write_header(f, total < VI_RING ? total : VI_RING, total);
        const uint64_t start = total > VI_RING ? total - VI_RING : 0;
        for (uint64_t i = start; i < total; i++) {
            vi_write_row(f, vi_ring[i % VI_RING]);
        }
        std::fclose(f);
    }

    void vi_record(const ultramodern::renderer::ViRegs* vi) {
        const uint64_t i = vi_samples.load();
        ViSample& v = vi_ring[i % VI_RING];
        v.n = i;
        v.ms = static_cast<uint32_t>(psrnx_ms_since_boot());
        v.dls = vi_dls_since_present.exchange(0);
        v.status = vi->VI_STATUS_REG;
        v.origin = vi->VI_ORIGIN_REG & 0xFFFFFF;
        v.width = vi->VI_WIDTH_REG;
        v.vstart = vi->VI_V_START_REG;
        v.hstart = vi->VI_H_START_REG;
        v.yscale = vi->VI_Y_SCALE_REG;
        v.xscale = vi->VI_X_SCALE_REG;
        v.vsync = vi->VI_V_SYNC_REG;
        v.ms_end = 0;
        v.has_pv = 0;
        std::memset(v.pv, 0, sizeof(v.pv));
        v.hash = 0;
        v.dy = 0;
        v.sad0 = v.sadbest = 0;
        if (i > 0) {
            const ViSample& pr = vi_ring[(i - 1) % VI_RING];
            if (pr.status != v.status || pr.width != v.width || pr.yscale != v.yscale || pr.xscale != v.xscale) last_mode_n = i;
        }
        if (i > 0 && !trans.active && trans.files < 12) {
            const ViSample& pr = vi_ring[(i - 1) % VI_RING];
            if (pr.status != v.status || pr.width != v.width || pr.yscale != v.yscale || pr.xscale != v.xscale) {
                trans.active = true;
                trans.first = i > 20 ? i - 20 : 0;
                trans.last = i + 120;
            }
        }
        // First time a mode (everything but the origin) shows up: log it.
        const uint32_t key[7] = {v.status, v.width, v.vstart, v.hstart, v.yscale, v.xscale, v.vsync};
        bool seen = false;
        for (uint32_t k = 0; k < vi_seen_n && !seen; k++) seen = std::memcmp(vi_seen[k], key, sizeof(key)) == 0;
        if (!seen && vi_seen_n < 64) {
            std::memcpy(vi_seen[vi_seen_n++], key, sizeof(key));
            std::printf("[PSRNX] vi mode #%u at present %llu: status=%08X width=%u vstart=%08X hstart=%08X yscale=%08X xscale=%08X vsync=%u origin=%06X\n",
                        vi_seen_n, static_cast<unsigned long long>(i), v.status, v.width, v.vstart, v.hstart, v.yscale, v.xscale, v.vsync, v.origin);
        }
        vi_samples.store(i + 1);
    }

    // ---- present probe ------------------------------------------------------------------------------------------------------
    // GLideN64 calls this from FrameBuffer.cpp right before it swaps (patch gliden64-viprobe.patch) with how the frame was composed.
    // With vi_probe=1 the finished picture is also read back: hash, vertical shift against the previous picture (row-profile match,
    // +dy = content moved down) and, every 600 presents, six consecutive pictures are kept as vi_frame_<set>_<i>.pgm.
    struct ProbeState {
        std::vector<uint8_t> rgb;
        std::vector<int64_t> prof, prev_prof;
        std::vector<uint8_t> gray;                 // current picture, top row first
        std::vector<std::vector<uint8_t>> kept;    // pictures of the running capture window
        std::vector<uint64_t> kept_n;
        int w = 0, h = 0;
        uint64_t count = 0;
        int window = -1;                           // -1: none running; else frames kept so far
        uint32_t set = 0;
    } probe;

    void probe_write_window() {
        char name[96];
        std::snprintf(name, sizeof(name), PSRNX_DATA_DIR "/vi_frame_%u.txt", probe.set & 1);
        if (FILE* m = std::fopen(name, "w")) {
            for (size_t i = 0; i < probe.kept.size(); i++) {
                const ViSample& v = vi_ring[probe.kept_n[i] % VI_RING];
                std::fprintf(m, "frame %zu present n=%llu ms=%u origin=%06X fb_start=%06X srcY0=%d dstY0=%d src=%d,%d,%d,%d dst=%d,%d,%d,%d hash=%016llX dy=%d\n",
                             i, static_cast<unsigned long long>(probe.kept_n[i]), v.ms, v.origin, static_cast<unsigned>(v.pv[1]),
                             v.pv[5], v.pv[6], v.pv[9], v.pv[10], v.pv[11], v.pv[12], v.pv[13], v.pv[14], v.pv[15], v.pv[16],
                             static_cast<unsigned long long>(v.hash), v.dy);
            }
            std::fclose(m);
        }
        for (size_t i = 0; i < probe.kept.size(); i++) {
            std::snprintf(name, sizeof(name), PSRNX_DATA_DIR "/vi_frame_%u_%zu.pgm", probe.set & 1, i);
            if (FILE* f = std::fopen(name, "wb")) {
                std::fprintf(f, "P5\n%d %d\n255\n", probe.w, probe.h);
                std::fwrite(probe.kept[i].data(), 1, probe.kept[i].size(), f);
                std::fclose(f);
            }
        }
        probe.kept.clear();
        probe.kept_n.clear();
        probe.set++;
    }

    void probe_picture(ViSample& v) {
        int w = 0, h = 0;
        ReadScreen2(nullptr, &w, &h, 0);
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
        probe.rgb.resize(static_cast<size_t>(w) * h * 3);
        ReadScreen2(probe.rgb.data(), &w, &h, 0);
        if (w != probe.w || h != probe.h) {
            probe.w = w;
            probe.h = h;
            probe.prev_prof.clear();
        }
        probe.gray.resize(static_cast<size_t>(w) * h);
        probe.prof.assign(h, 0);
        uint64_t hash = 1469598103934665603ull;
        for (int y = 0; y < h; y++) {
            // GL rows run bottom-up: row 0 of the picture is the last row read.
            const uint8_t* src = probe.rgb.data() + static_cast<size_t>(h - 1 - y) * w * 3;
            uint8_t* dst = probe.gray.data() + static_cast<size_t>(y) * w;
            int64_t sum = 0;
            for (int x = 0; x < w; x++) {
                const uint32_t g = (src[x * 3] * 77u + src[x * 3 + 1] * 150u + src[x * 3 + 2] * 29u) >> 8;
                dst[x] = static_cast<uint8_t>(g);
                sum += g;
                hash = (hash ^ g) * 1099511628211ull;
            }
            probe.prof[y] = sum;
        }
        v.hash = hash;
        if (probe.prev_prof.size() == static_cast<size_t>(h)) {
            uint64_t best = ~0ull, at0 = 0;
            int best_dy = 0;
            for (int dy = -8; dy <= 8; dy++) {
                uint64_t sum = 0;
                const int y0 = dy > 0 ? dy : 0, y1 = dy < 0 ? h + dy : h;
                for (int y = y0; y < y1; y++) sum += static_cast<uint64_t>(std::llabs(probe.prof[y] - probe.prev_prof[y - dy]));
                const uint64_t norm = sum / static_cast<uint64_t>(y1 - y0);
                if (dy == 0) at0 = norm;
                if (norm < best) { best = norm; best_dy = dy; }
            }
            v.dy = best_dy;
            v.sad0 = static_cast<uint32_t>(at0);
            v.sadbest = static_cast<uint32_t>(best);
        }
        probe.prev_prof = probe.prof;
        probe.count++;
        if (probe.window < 0 && probe.count % 600 == 0) probe.window = 0;
        if (probe.window >= 0) {
            probe.kept.push_back(probe.gray);
            probe.kept_n.push_back(v.n);
            if (++probe.window == 6) {
                probe.window = -1;
                // The PGM/text files are written after the window, from the samples of these presents.
                probe_write_window();
            }
        }
    }

    // ---- GB Tower view -------------------------------------------------------------------------------------------------------
    // GB Tower draws the Game Boy bezel into a 320x240 frame off to the left (about 76 px from the left edge and 220 px from the right
    // at 1280x720) and leaves the rest black. The bezel's box is found from one row and one column through the middle of the finished
    // picture (a few thousand pixels; reading the whole window back cost ~160 ms per call on this GPU and froze the picture every few
    // presents), checked twice before it is trusted, then each
    // present is re-composed with two blits (crop -> offscreen buffer -> window): gb_view 1 = bezel with the Game Boy's own proportions,
    // 2 = stretched over the whole window, 3 = into a centred 4:3 box. Only for the Game Boy screen (VI status without the dedither bit,
    // 320 wide, picture taken from RDRAM).
    struct GbView {
        using GenFn = void (*)(int, unsigned*);
        using BindFn = void (*)(unsigned, unsigned);
        using GetIntFn = void (*)(unsigned, int*);
        using GetBoolFn = void (*)(unsigned, unsigned char*);
        using GetFloatFn = void (*)(unsigned, float*);
        using EnableFn = void (*)(unsigned);
        using IsEnabledFn = unsigned char (*)(unsigned);
        void (*gen_fb)(int, unsigned*) = nullptr;
        void (*gen_rb)(int, unsigned*) = nullptr;
        void (*bind_fb)(unsigned, unsigned) = nullptr;
        void (*bind_rb)(unsigned, unsigned) = nullptr;
        void (*rb_storage)(unsigned, unsigned, int, int) = nullptr;
        void (*fb_rb)(unsigned, unsigned, unsigned, unsigned) = nullptr;
        unsigned (*fb_status)(unsigned) = nullptr;
        void (*blit)(int, int, int, int, int, int, int, int, unsigned, unsigned) = nullptr;
        void (*clear_color)(float, float, float, float) = nullptr;
        void (*clear)(unsigned) = nullptr;
        void (*color_mask)(unsigned char, unsigned char, unsigned char, unsigned char) = nullptr;
        void (*read_buffer)(unsigned) = nullptr;
        void (*pixel_store)(unsigned, int) = nullptr;
        void (*read_pixels)(int, int, int, int, unsigned, unsigned, void*) = nullptr;
        void (*get_int)(unsigned, int*) = nullptr;
        void (*get_bool)(unsigned, unsigned char*) = nullptr;
        void (*get_float)(unsigned, float*) = nullptr;
        void (*enable)(unsigned) = nullptr;
        void (*disable)(unsigned) = nullptr;
        unsigned char (*is_enabled)(unsigned) = nullptr;

        bool loaded = false, failed = false;
        unsigned fbo = 0, rbo = 0;
        int w = 0, h = 0;
        std::vector<uint8_t> line;   // centre row followed by centre column
        // Trusted bezel box (GL window pixels, bottom-up, inclusive) and the window size it was measured for.
        int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
        int box_w = 0, box_h = 0;
        bool have_box = false;
        // Candidate box that has to be seen by two probes in a row before it replaces the trusted one.
        int cx0 = 0, cy0 = 0, cx1 = -1, cy1 = -1, cand_n = 0;
        uint64_t next_probe = 0;     // VI sample count at which the next probe is due
        bool logged = false;
        // Cost bookkeeping, printed every 600 recomposed presents.
        uint32_t applies = 0, probes = 0;
        uint64_t apply_us = 0, apply_us_max = 0, probe_us = 0, probe_us_max = 0;
    } gbv;

    template <class F> F gl_proc(const char* name) { return reinterpret_cast<F>(SDL_GL_GetProcAddress(name)); }

    bool gb_view_load() {
        GbView& g = gbv;
        if (g.loaded || g.failed) return g.loaded;
#define PSRNX_GL(member, name) g.member = gl_proc<decltype(g.member)>(name); if (g.member == nullptr) { g.failed = true; }
        PSRNX_GL(gen_fb, "glGenFramebuffers") PSRNX_GL(gen_rb, "glGenRenderbuffers") PSRNX_GL(bind_fb, "glBindFramebuffer")
        PSRNX_GL(bind_rb, "glBindRenderbuffer") PSRNX_GL(rb_storage, "glRenderbufferStorage") PSRNX_GL(fb_rb, "glFramebufferRenderbuffer")
        PSRNX_GL(fb_status, "glCheckFramebufferStatus") PSRNX_GL(blit, "glBlitFramebuffer") PSRNX_GL(clear_color, "glClearColor")
        PSRNX_GL(clear, "glClear") PSRNX_GL(color_mask, "glColorMask") PSRNX_GL(read_buffer, "glReadBuffer") PSRNX_GL(pixel_store, "glPixelStorei")
        PSRNX_GL(read_pixels, "glReadPixels") PSRNX_GL(get_int, "glGetIntegerv") PSRNX_GL(get_bool, "glGetBooleanv")
        PSRNX_GL(get_float, "glGetFloatv") PSRNX_GL(enable, "glEnable") PSRNX_GL(disable, "glDisable") PSRNX_GL(is_enabled, "glIsEnabled")
#undef PSRNX_GL
        if (g.failed) { std::printf("[PSRNX] gb view: GL entry points missing, disabled\n"); return false; }
        g.loaded = true;
        return true;
    }

    // Offscreen colour buffer of the window's size; false if it cannot be made.
    bool gb_view_buffer(int w, int h) {
        GbView& g = gbv;
        if (g.fbo != 0 && g.w == w && g.h == h) return true;
        int old_fb = 0, old_rb = 0;
        g.get_int(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &old_fb);
        g.get_int(0x8CA7 /* RENDERBUFFER_BINDING */, &old_rb);
        if (g.fbo == 0) { g.gen_fb(1, &g.fbo); g.gen_rb(1, &g.rbo); }
        g.bind_rb(0x8D41, g.rbo);
        g.rb_storage(0x8D41, 0x8058 /* RGBA8 */, w, h);
        g.bind_fb(0x8CA9 /* DRAW_FRAMEBUFFER */, g.fbo);
        g.fb_rb(0x8CA9, 0x8CE0 /* COLOR_ATTACHMENT0 */, 0x8D41, g.rbo);
        const unsigned status = g.fb_status(0x8CA9);
        g.bind_fb(0x8CA9, static_cast<unsigned>(old_fb));
        g.bind_rb(0x8D41, static_cast<unsigned>(old_rb));
        if (status != 0x8CD5 /* FRAMEBUFFER_COMPLETE */) {
            std::printf("[PSRNX] gb view: offscreen buffer %dx%d incomplete (0x%X), disabled\n", w, h, status);
            g.failed = true;
            return false;
        }
        g.w = w; g.h = h;
        return true;
    }

    // Finds the bezel's box from one row and one column through the middle of the window's back buffer. Everything that is not
    // black on those two lines belongs to the bezel, so the first and last hits give its left/right and bottom/top edges.
    bool gb_view_probe(int w, int h, int* ox0, int* oy0, int* ox1, int* oy1) {
        GbView& g = gbv;
        int old_read_fb = 0, old_read_buf = 0, old_pack_buf = 0, old_align = 4;
        g.get_int(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &old_read_fb);
        g.get_int(0x0C02 /* READ_BUFFER */, &old_read_buf);
        g.get_int(0x88ED /* PIXEL_PACK_BUFFER_BINDING */, &old_pack_buf);
        g.get_int(0x0D05 /* PACK_ALIGNMENT */, &old_align);
        g.bind_fb(0x8CA8 /* READ_FRAMEBUFFER */, 0);
        g.read_buffer(0x0405 /* BACK */);
        g.pixel_store(0x0D05, 1);
        const bool pack_bind = old_pack_buf != 0;
        using BindBufFn = void (*)(unsigned, unsigned);
        const auto bind_buf = gl_proc<BindBufFn>("glBindBuffer");
        if (pack_bind && bind_buf) bind_buf(0x88EB /* PIXEL_PACK_BUFFER */, 0);
        g.line.resize(static_cast<size_t>(w) + static_cast<size_t>(h));
        g.read_pixels(0, h / 2, w, 1, 0x1903 /* RED */, 0x1401 /* UNSIGNED_BYTE */, g.line.data());
        g.read_pixels(w / 2, 0, 1, h, 0x1903, 0x1401, g.line.data() + w);
        if (pack_bind && bind_buf) bind_buf(0x88EB, static_cast<unsigned>(old_pack_buf));
        g.pixel_store(0x0D05, old_align);
        g.read_buffer(static_cast<unsigned>(old_read_buf));
        g.bind_fb(0x8CA8, static_cast<unsigned>(old_read_fb));

        int x0 = -1, x1 = -1, y0 = -1, y1 = -1;
        for (int x = 0; x < w; x++) if (g.line[x] > 16) { if (x0 < 0) x0 = x; x1 = x; }
        for (int y = 0; y < h; y++) if (g.line[w + y] > 16) { if (y0 < 0) y0 = y; y1 = y; }
        if (x0 < 0 || y0 < 0) return false;
        const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        // The bezel is about 3:2 and fills most of the height; anything else is a transition or a loading picture.
        if (bw < w / 4 || bh < h / 2 || bw * 10 < bh * 11 || bw * 10 > bh * 20) return false;
        *ox0 = x0; *oy0 = y0; *ox1 = x1; *oy1 = y1;
        return true;
    }

    // Runs a probe and updates the trusted box once two probes in a row agree on a new one.
    void gb_view_track(int w, int h, uint64_t n) {
        GbView& g = gbv;
        const auto t0 = std::chrono::steady_clock::now();
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool ok = gb_view_probe(w, h, &x0, &y0, &x1, &y1);
        const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        g.probes++; g.probe_us += us; if (us > g.probe_us_max) g.probe_us_max = us;
        if (g.probes <= 4) std::printf("[PSRNX] gb view: probe %u took %llu us (%s)\n", g.probes, static_cast<unsigned long long>(us), ok ? "bezel found" : "no bezel");
        // A box that fills the window is nothing to crop. It is not trusted as a first box (a white loading picture would
        // otherwise hold off the real one), but it is accepted as a change once a box exists (a full-screen overlay).
        if (ok && !g.have_box && (x1 - x0 + 1) >= w * 98 / 100 && (y1 - y0 + 1) >= h * 98 / 100) ok = false;
        if (!ok) {
            g.cand_n = 0;
            g.next_probe = n + (g.have_box ? 120 : 10);
            return;
        }
        const auto close_to = [](int a, int b) { return std::abs(a - b) <= 3; };
        if (g.cand_n > 0 && close_to(x0, g.cx0) && close_to(y0, g.cy0) && close_to(x1, g.cx1) && close_to(y1, g.cy1)) {
            g.cand_n++;
        } else {
            g.cx0 = x0; g.cy0 = y0; g.cx1 = x1; g.cy1 = y1; g.cand_n = 1;
        }
        if (g.cand_n >= 2) {
            g.bx0 = g.cx0; g.by0 = g.cy0; g.bx1 = g.cx1; g.by1 = g.cy1;
            g.box_w = w; g.box_h = h;
            g.have_box = true;
            g.cand_n = 0;
            g.next_probe = n + 120;
        } else {
            g.next_probe = n + 4;
        }
    }

    // Adds the time spent in gb_view_apply to the running totals and prints them now and then.
    struct GbApplyTimer {
        GbView& g;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        explicit GbApplyTimer(GbView& gv) : g(gv) {}
        ~GbApplyTimer() {
            const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            g.applies++; g.apply_us += us; if (us > g.apply_us_max) g.apply_us_max = us;
            if (g.applies >= 600) {
                std::printf("[PSRNX] gb view cost: %u presents, %llu us each on average, %llu us at most; %u probes, %llu us each on average, %llu us at most\n",
                            g.applies, static_cast<unsigned long long>(g.apply_us / g.applies), static_cast<unsigned long long>(g.apply_us_max),
                            g.probes, static_cast<unsigned long long>(g.probes ? g.probe_us / g.probes : 0), static_cast<unsigned long long>(g.probe_us_max));
                g.applies = 0; g.apply_us = 0; g.apply_us_max = 0; g.probes = 0; g.probe_us = 0; g.probe_us_max = 0;
            }
        }
    };

    // Re-composes the finished GB Tower picture. Called right before the swap.
    void gb_view_apply(const int* pv, uint32_t status) {
        const int mode = psrnx::config().gb_view;
        if (mode <= 0 || (status & 0x10000) != 0 || pv[28] != 320 || pv[31] != 1) return;
        const int w = pv[26], h = pv[25];
        if (w < 64 || h < 64 || !gb_view_load()) return;
        GbView& g = gbv;
        if (g.failed) return;
        if (!gb_view_buffer(w, h)) return;
        GbApplyTimer timer(g);
        const uint64_t n = vi_samples.load();
        if (g.have_box && (g.box_w != w || g.box_h != h)) { g.have_box = false; g.cand_n = 0; }   // window size changed
        if (n >= g.next_probe) gb_view_track(w, h, n);
        if (!g.have_box) return;
        const int bw = g.bx1 - g.bx0 + 1, bh = g.by1 - g.by0 + 1;
        if (bw >= w * 98 / 100 && bh >= h * 98 / 100) return;   // already fills the window
        int dw = w, dh = h;
        if (mode == 1) {
            const float s = std::min(static_cast<float>(w) / bw, static_cast<float>(h) / bh);
            dw = static_cast<int>(bw * s); dh = static_cast<int>(bh * s);
        } else if (mode == 3) {
            dh = h; dw = std::min(w, h * 4 / 3);
        }
        const int dx = (w - dw) / 2, dy = (h - dh) / 2;
        if (!g.logged) {
            g.logged = true;
            std::printf("[PSRNX] gb view: status=%08X window %dx%d bezel box x=%d..%d y=%d..%d (top-down y=%d..%d) -> mode %d into %dx%d at %d,%d\n",
                        status, w, h, g.bx0, g.bx1, g.by0, g.by1, h - 1 - g.by1, h - 1 - g.by0, mode, dw, dh, dx, dy);
        }

        int old_read = 0, old_draw = 0, old_rb = 0;
        unsigned char old_mask[4] = {1, 1, 1, 1}, scissor = g.is_enabled(0x0C11 /* SCISSOR_TEST */);
        float old_clear[4] = {0, 0, 0, 0};
        int old_read_buf = 0;
        g.get_int(0x0C02, &old_read_buf);
        g.get_int(0x8CAA, &old_read);
        g.get_int(0x8CA6, &old_draw);
        g.get_bool(0x0C23 /* COLOR_WRITEMASK */, old_mask);
        g.get_float(0x0C22 /* COLOR_CLEAR_VALUE */, old_clear);
        (void)old_rb;
        g.disable(0x0C11);
        g.color_mask(1, 1, 1, 1);
        // black buffer, then the cropped bezel scaled into it
        g.bind_fb(0x8CA9, g.fbo);
        g.clear_color(0.f, 0.f, 0.f, 1.f);
        g.clear(0x4000 /* COLOR_BUFFER_BIT */);
        g.bind_fb(0x8CA8, 0);
        g.read_buffer(0x0405 /* BACK */);
        g.blit(g.bx0, g.by0, g.bx1 + 1, g.by1 + 1, dx, dy, dx + dw, dy + dh, 0x4000, 0x2601 /* LINEAR */);
        // and the buffer back onto the window, 1:1
        g.bind_fb(0x8CA8, g.fbo);
        g.bind_fb(0x8CA9, 0);
        g.blit(0, 0, w, h, 0, 0, w, h, 0x4000, 0x2600 /* NEAREST */);
        g.bind_fb(0x8CA8, static_cast<unsigned>(old_read));
        g.bind_fb(0x8CA9, static_cast<unsigned>(old_draw));
        g.read_buffer(static_cast<unsigned>(old_read_buf));
        g.color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
        g.clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
        if (scissor) g.enable(0x0C11);
    }

    void present_hook(const int* pv) {
        const uint64_t total = vi_samples.load();
        if (total == 0) return;
        ViSample& v = vi_ring[(total - 1) % VI_RING];
        std::memcpy(v.pv, pv, sizeof(v.pv));
        v.has_pv = 1;
        if (psrnx::config().vi_probe) probe_picture(v);
        const int hold_max = psrnx::config().vi_hold;
        if (hold_max > 0 && pv[31] == 1 && good_shown && total - 1 - last_mode_n < static_cast<uint64_t>(hold_max)) {
            psrnx_skip_swap = 1;
            v.hash = 0x4845'4C44ull;  // 'HELD' in the ring's hash column (probe off)
            hold_run++;
            hold_total++;
        } else {
            if (hold_run > 0)
                std::printf("[PSRNX] vi hold: kept the previous picture for %u presents after the mode change at present %llu (%u in total)\n",
                            hold_run, static_cast<unsigned long long>(last_mode_n), hold_total);
            hold_run = 0;
            if (pv[31] == 0) good_shown = true;
        }
        if (!psrnx_skip_swap) gb_view_apply(pv, v.status);
        v.ms_end = static_cast<uint32_t>(psrnx_ms_since_boot());
        if (trans.active && total - 1 >= trans.last) trans_finish();
    }

    // ultramodern signals SP/DP completion itself once send_dl returns.
    void check_interrupts() {}

    void debug_callback(void*, int level, const char* message) {
        if (level <= M64MSG_WARNING) {
            std::fprintf(stderr, "[GLideN64] %s\n", message);
        }
    }

    // GLideN64 reads the header as 32-bit host-order words, like the ROM image mupen64plus keeps.
    void build_rom_header() {
        std::memset(rom_header, 0, sizeof(rom_header));
        for (size_t i = 0; i < sizeof(ROM_NAME) - 1; i++) {
            rom_header[(0x20 + i) ^ 3] = static_cast<uint8_t>(ROM_NAME[i]);
        }
    }

    uint32_t to_physical(uint32_t address) {
        return address & KSEG_MASK;
    }

    class GLideN64RendererContext final : public ultramodern::renderer::RendererContext {
    public:
        explicit GLideN64RendererContext(uint8_t* rdram) {
            chosen_api = ultramodern::renderer::GraphicsApi::Auto;
            psrnx_seed_gliden64_config();
            build_rom_header();

            SplashGuard splash_guard;   // picture while the shader warm-up runs; ends when the constructor returns
            psrnx_vi_field_mode = psrnx::config().vi_field;
            psrnx_shader_warm_ms = psrnx::config().shader_warm_ms;
            // Stamp: proves which binary produced a log (compile time of this file) and what the shader warm-up was told.
            std::printf("[PSRNX] build stamp: render_gliden64.cpp compiled %s %s, shader_warm_ms=%d\n", __DATE__, __TIME__, psrnx_shader_warm_ms);
            if (PluginStartup(nullptr, nullptr, debug_callback) != M64ERR_SUCCESS) {
                setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
                return;
            }

            auto* vi = ultramodern::renderer::get_vi_regs();
            GFX_INFO info{};
            info.HEADER = rom_header;
            info.RDRAM = rdram;
            info.DMEM = dmem;
            info.IMEM = imem;
            info.MI_INTR_REG = &mi_intr_reg;
            info.DPC_START_REG = &dpc_start_reg;
            info.DPC_END_REG = &dpc_end_reg;
            info.DPC_CURRENT_REG = &dpc_current_reg;
            info.DPC_STATUS_REG = &dpc_status_reg;
            info.DPC_CLOCK_REG = &dpc_clock_reg;
            info.DPC_BUFBUSY_REG = &dpc_bufbusy_reg;
            info.DPC_PIPEBUSY_REG = &dpc_pipebusy_reg;
            info.DPC_TMEM_REG = &dpc_tmem_reg;
            info.VI_STATUS_REG = &vi->VI_STATUS_REG;
            info.VI_ORIGIN_REG = &vi->VI_ORIGIN_REG;
            info.VI_WIDTH_REG = &vi->VI_WIDTH_REG;
            info.VI_INTR_REG = &vi->VI_INTR_REG;
            info.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
            info.VI_TIMING_REG = &vi->VI_TIMING_REG;
            info.VI_V_SYNC_REG = &vi->VI_V_SYNC_REG;
            info.VI_H_SYNC_REG = &vi->VI_H_SYNC_REG;
            info.VI_LEAP_REG = &vi->VI_LEAP_REG;
            info.VI_H_START_REG = &vi->VI_H_START_REG;
            info.VI_V_START_REG = &vi->VI_V_START_REG;
            info.VI_V_BURST_REG = &vi->VI_V_BURST_REG;
            info.VI_X_SCALE_REG = &vi->VI_X_SCALE_REG;
            info.VI_Y_SCALE_REG = &vi->VI_Y_SCALE_REG;
            info.CheckInterrupts = check_interrupts;
            info.version = 2;
            info.SP_STATUS_REG = &sp_status_reg;
            info.RDRAM_SIZE = &rdram_size;

            if (!InitiateGFX(info) || !RomOpen()) {
                setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
                return;
            }
            // The warm-up inside InitiateGFX/RomOpen survived: the lock only exists if the extras were really built this time.
            if (std::remove(PSRNX_DATA_DIR "/shader_warm.lock") == 0) std::remove(PSRNX_DATA_DIR "/shader_warm_crashed.txt");
            setup_result = ultramodern::renderer::SetupResult::Success;
            opened = true;
            psrnx_present_hook = &present_hook;
            log_gl_info();
            std::thread([]() {
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    vi_dump();
                    shader_info_dump();
                }
            }).detach();
        }

        ~GLideN64RendererContext() override {
            shutdown();
        }

        bool valid() override { return opened; }

        bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override {
            return true;
        }

        void enable_instant_present() override {}

        void send_dl(const OSTask* task) override {
            // Lay the task out in DMEM as osSpTaskLoad does, with physical addresses.
            const OSTask_s& t = task->t;
            const auto u = [](auto value) { return static_cast<uint32_t>(value); };
            const uint32_t words[16] = {
                u(t.type), u(t.flags),
                to_physical(u(t.ucode_boot)), u(t.ucode_boot_size),
                to_physical(u(t.ucode)), u(t.ucode_size),
                to_physical(u(t.ucode_data)), u(t.ucode_data_size),
                to_physical(u(t.dram_stack)), u(t.dram_stack_size),
                to_physical(u(t.output_buff)), u(t.output_buff_size),
                to_physical(u(t.data_ptr)), u(t.data_size),
                to_physical(u(t.yield_data_ptr)), u(t.yield_data_size),
            };
            const uint64_t n = psrnx_diag::send_dl.fetch_add(1) + 1;
            vi_dls_since_present.fetch_add(1);
            if (n <= 5) {
                std::printf("[PSRNX] send_dl #%llu: type=%u flags=%u ucode=0x%08X data=0x%08X size=%u\n",
                             static_cast<unsigned long long>(n), words[0], words[1], words[4], words[12], words[13]);
            }
            std::memcpy(&dmem[TASK_DMEM_OFFSET], words, sizeof(words));
            // HALT | BROKE | TASKDONE, set before ProcessDList as GFX_INFO v2 requires.
            sp_status_reg = 0x0203;
            const auto t0 = std::chrono::steady_clock::now();
            psrnx_diag::in_process_dl.store(true, std::memory_order_relaxed);
            ProcessDList();
            psrnx_diag::in_process_dl.store(false, std::memory_order_relaxed);
            const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            psrnx_diag::send_dl_ns.fetch_add(ns, std::memory_order_relaxed);
            if (ns > 100'000'000ull && psrnx_diag::slow_calls.fetch_add(1) < 80) {
                std::printf("[PSRNX] SLOW ProcessDList #%llu: %llu ms ucode=0x%08X data=0x%08X size=%u (t=%llu ms)\n",
                             static_cast<unsigned long long>(n), static_cast<unsigned long long>(ns / 1000000),
                             words[4], words[12], words[13], psrnx_ms_since_boot());
            }
        }

        void update_screen() override {
            const uint64_t n = psrnx_diag::update_screen.fetch_add(1) + 1;
            vi_record(ultramodern::renderer::get_vi_regs());
            if (n <= 3) std::printf("[PSRNX] update_screen #%llu\n", static_cast<unsigned long long>(n));
            const auto t0 = std::chrono::steady_clock::now();
            UpdateScreen();
            const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            psrnx_diag::update_screen_ns.fetch_add(ns, std::memory_order_relaxed);
            // A present normally blocks for one vsync (~16 ms); anything far beyond that is a readback or a stall.
            if (ns > 100'000'000ull && psrnx_diag::slow_calls.fetch_add(1) < 80) {
                std::printf("[PSRNX] SLOW UpdateScreen #%llu: %llu ms (t=%llu ms)\n",
                             static_cast<unsigned long long>(n), static_cast<unsigned long long>(ns / 1000000),
                             psrnx_ms_since_boot());
            }
        }

        void shutdown() override {
            if (opened) {
                RomClosed();
                PluginShutdown();
                opened = false;
            }
        }

        uint32_t get_display_framerate() const override { return 60; }
        float get_resolution_scale() const override { return static_cast<float>(psrnx::config().resolution_factor); }

    private:
        bool opened = false;
    };
}

std::unique_ptr<ultramodern::renderer::RendererContext> psrnx_create_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
    return std::make_unique<GLideN64RendererContext>(rdram);
}
