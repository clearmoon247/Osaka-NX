// Switch entry point for Pokemon Stadium: Recompiled. Mirrors src/main/main.cpp's boot sequence without the PC
// frontend (RT64, recomp-ui launcher, Win32 pickers, TCP debug server, Ares worker, WASAPI): the game boots straight
// from the ROM placed in sdmc:/switch/osaka-nx/.
//
// Written against mstan/N64ModernRuntime d4bf882 (recomp::start takes positional arguments there), following the structure of
// DK64Recomp-NX's switch/main_switch.cpp (https://github.com/Trinalord777/DK64Recomp-NX, GPL-3.0).
#include <algorithm>
#include <atomic>
#include <map>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

#include <unistd.h>

#include <SDL.h>
#include <switch.h>
#include <sys/stat.h>

#include "librecomp/game.hpp"
#include "librecomp/gbcart.hpp"
#include "librecomp/rsp.hpp"
#include "recomp.h"
#include "ultramodern/config.hpp"
#include "ultramodern/error_handling.hpp"
#include "ultramodern/events.hpp"
#include "ultramodern/input.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/ultra_trace.hpp"

#include "../src/main/app_paths.h"
#include "../src/main/transfer_pak.h"
#include "data_migration.h"
#include "psrnx.h"
#include "psrnx_sample.h"

// nvdrv keeps the bookkeeping of every GPU allocation (Mesa makes one nvMapCreate per texture) in transfer memory
// libnx hands it at start-up: 8 MiB by default. DK64Recomp-NX measured GL_OUT_OF_MEMORY near 5,000 live textures with
// the default; 32 MiB holds about 20,000. Stadium is texture-heavy (Pokemon models), so keep the larger value.
extern "C" u32 __nx_nv_transfermem_size;
u32 __nx_nv_transfermem_size = 32 * 1024 * 1024;

SDL_Window* psrnx_window = nullptr;

// Provided by the other switch/ files.
std::u8string psrnx_register_stadium();                              // stadium_setup.cpp
RspUcodeFunc* psrnx_get_rsp_microcode(uint8_t*, const OSTask*);      // rsp_select.cpp
void psrnx_queue_samples(int16_t*, size_t);                          // audio_switch.cpp
size_t psrnx_get_frames_remaining();
void psrnx_set_frequency(uint32_t);
bool psrnx_reset_audio(uint32_t);
void psrnx_close_audio();
void psrnx_input_handle_event(const SDL_Event&);                     // input_switch.cpp
void psrnx_poll_input();
bool psrnx_get_input(int, uint16_t*, float*, float*);
void psrnx_set_rumble(int, bool);
void psrnx_update_rumble();

extern "C" {
uint64_t ultramodern_submit_gfx_count(void);
uint64_t ultramodern_submit_audio_count(void);
uint64_t ultramodern_submit_other_count(void);
uint64_t ultramodern_sp_complete_count(void);
uint64_t ultramodern_dp_complete_count(void);
void recomp_coverage_live(uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*);  // overlays.cpp
void ultramodern_mesg_recent_copy(void*, size_t, size_t*, uint64_t*);
size_t ultramodern_mesg_event_size(void);
size_t ultramodern_mesg_qstate_size(void);
void ultramodern_mesg_qstates_copy(void*, size_t, size_t*);
void ultramodern_sched_recent_copy(void*, size_t, size_t*, uint64_t*);
size_t ultramodern_sched_event_size(void);
uint64_t ultramodern_external_requeues(void);   // mesgqueue.cpp: reliable host messages that found their queue full
uint32_t ultramodern_external_parked(void);     // ... and how many are parked right now
void psrnx_place_thread(const char*);                                                                        // threads_switch.c
extern volatile uint32_t psrnx_loop_pc;      // threads_switch.c: last loop back-edge any guest thread passed
extern volatile uint64_t psrnx_loop_ticks;   // ... and how many back-edges so far
uintptr_t psrnx_code_marker(void);
void psrnx_thread_stats(uint64_t* created, uint64_t* reaped, uint64_t* tracked);   // threads_switch.c
void recomp_interp_stats(uint64_t*, uint64_t*);                                                             // mips_interp.cpp
int pkmnstadium_gbtower_trace_dump(uint8_t*, const char*, int);   // extras.c: GB Tower emulator state + recent checkpoints
void psrnx_gbtower_state_line(uint8_t*, char*, size_t);            // gbtower_dump_switch.c (build 27 diagnostics)
int psrnx_gbtower_snapshot(uint8_t*, const char*);
extern volatile uint64_t psrnx_gb_dispatch_count;
extern volatile uint64_t psrnx_gb_init_count;
uint64_t pkmnstadium_gbtower_trace_seq(void);
void recomp_rsp_force_sig3(uint8_t*);                             // librecomp rsp.cpp (patch n64modernruntime-sigheal)
}
bool psrnx_run_gb_picker(std::string&);   // gbpicker_switch.cpp (C++ linkage, so outside the extern "C" block)

namespace psrnx_diag {
    std::atomic<uint64_t> vi_ticks{0}, send_dl{0}, update_screen{0}, audio_pushes{0}, audio_frames{0},
        send_dl_ns{0}, update_screen_ns{0}, slow_calls{0};
    std::atomic<bool> in_process_dl{false};
}
ultramodern::input::connected_device_info_t psrnx_get_connected_device_info(int);
std::unique_ptr<ultramodern::renderer::RendererContext> psrnx_create_render_context(  // render_gliden64.cpp
    uint8_t*, ultramodern::renderer::WindowHandle, bool);
extern "C" void psrnx_window_set_size(int, int);                     // window_switch.c

// Milliseconds since the console booted, for the start-up timeline in the log (also core_switch.cpp).
extern "C" unsigned long long psrnx_ms_since_boot() { return armTicksToNs(armGetSystemTick()) / 1000000; }

// app_paths.h contract: runtime files live in the data directory on the SD card.
namespace pkmnstadium {
    std::filesystem::path exe_dir() { return PSRNX_DATA_DIR; }
    std::filesystem::path app_file(const std::string& name) { return exe_dir() / name; }
}
extern "C" const char* psr_app_file_c(const char* name, char* out, unsigned long out_size) {
    if (out == nullptr || out_size == 0) return "";
    const std::string path = pkmnstadium::app_file(name != nullptr ? name : "").string();
    std::snprintf(out, out_size, "%s", path.c_str());
    return out;
}

namespace {
    void step(const char* what) {
        std::printf("[PSRNX] step: %s (%llu ms)\n", what, psrnx_ms_since_boot());
    }

    [[noreturn]] void log_terminate() {
        try {
            if (auto current = std::current_exception()) std::rethrow_exception(current);
            std::fprintf(stderr, "[PSRNX] std::terminate without an active exception\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[PSRNX] uncaught exception: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[PSRNX] uncaught non-standard exception\n");
        }
        svcSleepThread(2000000000ULL);
        std::abort();
    }

    // Last events of the runtime's message-queue ring and scheduler ring (ultramodern/src/mesgqueue.cpp, threadqueue.cpp).
    // mesg ops: 1 send(game) 2 send(external) 3 recv enter 4 recv block 5 recv ok 6 ext delivered 7 ext requeued (target
    // full) 8 send blocked 9 ext dropped (coalesced VI on a full queue) 10 NOBLOCK send dropped.
    struct MesgEv { uint64_t seq, ms; uint32_t mq, msg, thread; uint16_t tid, vb, va; uint8_t op, block, game, pad; uint16_t res; };
    struct SchedEv { uint64_t seq, ms; uint32_t op, queue, thread, cur, head_after, next_after; uint16_t tid, cur_tid; int16_t pri; uint16_t pad; };
    // Per-queue history (never evicted): for every queue whose newest event is a blocked receive (a parked thread), plus the
    // queues named in `extra`, print its last events. Answers "who sent to this queue, and when?".
    struct QStateEv { uint32_t queue, count; MesgEv last[64]; };
    void dump_qstates(const uint32_t* extra, int n_extra) {
        if (ultramodern_mesg_qstate_size() != sizeof(QStateEv)) {
            std::printf("[PSRNX] qstate layout differs (size %zu)\n", ultramodern_mesg_qstate_size());
            return;
        }
        std::vector<QStateEv> q(1024);
        size_t n = 0;
        ultramodern_mesg_qstates_copy(q.data(), q.size(), &n);
        std::printf("[PSRNX] queue table: %zu queues\n", n);
        for (size_t i = 0; i < n; ++i) {
            const QStateEv& e = q[i];
            const MesgEv& newest = e.last[(e.count - 1) & 63];
            bool want = newest.op == 4;
            for (int k = 0; k < n_extra; ++k) want = want || e.queue == extra[k];
            std::printf("[PSRNX]   queue %08X: %u events, newest op=%u at +%llums by tid %u%s\n", e.queue, e.count, newest.op,
                        static_cast<unsigned long long>(newest.ms), newest.tid, want ? " (detail)" : "");
            if (!want) continue;
            const uint32_t take = e.count < 14 ? e.count : 14;
            for (uint32_t j = take; j > 0; --j) {
                const MesgEv& m = e.last[(e.count - j) & 63];
                std::printf("[PSRNX]       +%llums op=%u msg=%08X tid=%u valid %u->%u blk=%u\n", static_cast<unsigned long long>(m.ms),
                            m.op, m.msg, m.tid, m.vb, m.va, m.block);
            }
        }
    }

    void dump_rings(size_t n) {
        static MesgEv mv[512];
        static SchedEv sv[512];
        size_t got = 0; uint64_t nseq = 0;
        if (ultramodern_mesg_event_size() == sizeof(MesgEv) && n <= 512) {
            ultramodern_mesg_recent_copy(mv, n, &got, &nseq);
            std::printf("[PSRNX] mesg ring: %llu events total, last %zu:\n", static_cast<unsigned long long>(nseq), got);
            for (size_t i = 0; i < got; ++i)
                std::printf("[PSRNX]   m#%llu +%llums op=%u mq=%08X msg=%08X tid=%u valid %u->%u blk=%u game=%u\n",
                            static_cast<unsigned long long>(mv[i].seq), static_cast<unsigned long long>(mv[i].ms), mv[i].op, mv[i].mq,
                            mv[i].msg, mv[i].tid, mv[i].vb, mv[i].va, mv[i].block, mv[i].game);
        } else {
            std::printf("[PSRNX] mesg ring layout differs (event size %zu)\n", ultramodern_mesg_event_size());
        }
        if (ultramodern_sched_event_size() == sizeof(SchedEv) && n <= 512) {
            ultramodern_sched_recent_copy(sv, n, &got, &nseq);
            std::printf("[PSRNX] sched ring: %llu events total, last %zu:\n", static_cast<unsigned long long>(nseq), got);
            for (size_t i = 0; i < got; ++i)
                std::printf("[PSRNX]   s#%llu +%llums op=%u queue=%08X thread=%08X(id %u pri %d) cur_id=%u head_after=%08X\n",
                            static_cast<unsigned long long>(sv[i].seq), static_cast<unsigned long long>(sv[i].ms), sv[i].op, sv[i].queue,
                            sv[i].thread, sv[i].tid, sv[i].pri, sv[i].cur_tid, sv[i].head_after);
        } else {
            std::printf("[PSRNX] sched ring layout differs (event size %zu)\n", ultramodern_sched_event_size());
        }
    }

    // Dumps the runtime's always-on libultra call ring (ultramodern/ultra_trace.hpp): a histogram of which calls each guest
    // thread made recently (a thread hammering one call is spinning, a thread absent from the list is blocked) and the
    // raw tail. Names starting with '~' are events the runtime delivered to the game (VI retrace, SP/DP done, ...).
    void dump_trace(const char* label, uint32_t window, uint32_t raw_tail) {
        const uint64_t widx = recomp_ultra_trace_write_idx();
        std::printf("[PSRNX] trace %s: %llu events recorded\n", label, static_cast<unsigned long long>(widx));
        std::map<std::pair<unsigned, std::string>, unsigned> histogram;
        recomp_ultra_trace_event ev{};
        const uint64_t first = widx > window ? widx - window : 0;
        for (uint64_t i = first; i < widx; ++i) {
            if (recomp_ultra_trace_get(i, &ev)) histogram[{ev.thread_id, ev.name}]++;
        }
        std::vector<std::pair<unsigned, std::pair<unsigned, std::string>>> sorted;
        for (const auto& [key, count] : histogram) sorted.push_back({count, key});
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < sorted.size() && i < 40; ++i) {
            std::printf("[PSRNX]   x%-5u thread %-3u %s\n", sorted[i].first, sorted[i].second.first, sorted[i].second.second.c_str());
        }
        const uint64_t tail_first = widx > raw_tail ? widx - raw_tail : 0;
        for (uint64_t i = tail_first; i < widx; ++i) {
            if (recomp_ultra_trace_get(i, &ev)) {
                std::printf("[PSRNX]   #%llu +%llums t%u pc=%08X %s a0=%08X a1=%08X a2=%08X a3=%08X\n",
                            static_cast<unsigned long long>(ev.seq), static_cast<unsigned long long>(ev.ms),
                            static_cast<unsigned>(ev.thread_id), ev.pc, ev.name, ev.a0, ev.a1, ev.a2, ev.a3);
            }
        }
    }

    void dump_boot_trace(uint32_t count) {
        const uint32_t have = recomp_ultra_trace_boot_count();
        std::printf("[PSRNX] boot trace: first %u of %u events\n", count < have ? count : have, have);
        recomp_ultra_trace_event ev{};
        for (uint32_t i = 0; i < have && i < count; ++i) {
            if (recomp_ultra_trace_boot_get(i, &ev)) {
                std::printf("[PSRNX]   b%u +%llums t%u pc=%08X %s a0=%08X a1=%08X a2=%08X\n", i,
                            static_cast<unsigned long long>(ev.ms), static_cast<unsigned>(ev.thread_id), ev.pc, ev.name,
                            ev.a0, ev.a1, ev.a2);
            }
        }
    }

    // Per guest thread: its OSThread as the runtime sees it (priority, state, the queue it waits on) and the last libultra
    // call it made. A thread whose last call is an osRecvMesg with no ".done" after it is blocked on that queue (a0).
    void dump_threads() {
        uint8_t* rdram = recomp_runtime_get_rdram();
        if (rdram == nullptr) return;
        struct Seen { uint32_t ptr = 0, entry = 0; uint64_t events = 0; recomp_ultra_trace_event last{}; bool any = false; };
        std::map<unsigned, Seen> threads;
        auto visit = [&](const recomp_ultra_trace_event& ev) {
            Seen& t = threads[ev.thread_id];
            t.events++;
            t.last = ev;
            t.any = true;
            if (std::strcmp(ev.name, "osCreateThread_recomp") == 0) {
                Seen& created = threads[ev.a1];
                created.ptr = ev.a0;
                created.entry = ev.a2;
            }
        };
        recomp_ultra_trace_event ev{};
        const uint32_t boot = recomp_ultra_trace_boot_count();
        for (uint32_t i = 0; i < boot; ++i) {
            if (recomp_ultra_trace_boot_get(i, &ev)) visit(ev);
        }
        const uint64_t widx = recomp_ultra_trace_write_idx();
        const uint64_t cap = recomp_ultra_trace_capacity();
        for (uint64_t i = std::max<uint64_t>(widx > cap ? widx - cap : 0, boot); i < widx; ++i) {
            if (recomp_ultra_trace_get(i, &ev)) visit(ev);
        }
        std::printf("[PSRNX] threads (OSThread state 0=stopped 1=queued 2=running 3=blocked):\n");
        for (const auto& [id, t] : threads) {
            if (t.ptr != 0) {
                const OSThread* os = reinterpret_cast<const OSThread*>(rdram + (t.ptr - 0x80000000u));
                std::printf("[PSRNX]   thread %-3u os=%08X entry=%08X pri=%d state=%u queue=%08X\n", id, t.ptr, t.entry,
                            static_cast<int>(os->priority), static_cast<unsigned>(os->state),
                            static_cast<uint32_t>(os->queue));
            } else {
                std::printf("[PSRNX]   thread %-3u (no osCreateThread seen)\n", id);
            }
            if (t.any) {
                std::printf("[PSRNX]       %llu events; last: #%llu +%llums pc=%08X %s a0=%08X a1=%08X a2=%08X\n",
                            static_cast<unsigned long long>(t.events), static_cast<unsigned long long>(t.last.seq),
                            static_cast<unsigned long long>(t.last.ms), t.last.pc, t.last.name, t.last.a0, t.last.a1,
                            t.last.a2);
            }
        }
    }

    // Stadium's scheduler (D_800A62E0, src/5580.c) as the game sees it: which task is on the RSP (A14), the audio task (A18),
    // the current/pending graphics tasks (A1C/A24) and each task's state words (unk_1C: 0 new, 1 running, 2 yielding, 3 RSP done;
    // unk_1E: 1 RDP pending, 2 RDP done). A graphics thread waiting for a "DONE" that never comes shows up here as a task
    // that is stuck in a state the other side never completes.
    void dump_sched_state() {
        uint8_t* rdram = recomp_runtime_get_rdram();
        if (rdram == nullptr) return;
        auto ok = [](uint32_t a) { return a >= 0x80000000u && a < 0x80800000u; };
        auto w32 = [&](uint32_t a) -> uint32_t { return ok(a) ? *reinterpret_cast<const uint32_t*>(rdram + (a - 0x80000000u)) : 0xDEADBEEFu; };
        auto w16 = [&](uint32_t a) -> uint32_t { return ok(a) ? *reinterpret_cast<const uint16_t*>(rdram + ((a - 0x80000000u) ^ 2u)) : 0xDEADu; };
        const uint32_t S = 0x800A62E0u;
        std::printf("[PSRNX] sched state: A10=%08X A14(rsp)=%08X A18(aud)=%08X A1C(gfx)=%08X A20=%08X A24(gfx next)=%08X "
                    "A28=%d A2C=%d A30=%d A34(frames)=%u A38(prenmi)=%d  state=0x%X flag800A83A0=%u\n",
                    w32(S + 0xA10), w32(S + 0xA14), w32(S + 0xA18), w32(S + 0xA1C), w32(S + 0xA20), w32(S + 0xA24),
                    static_cast<int>(w32(S + 0xA28)), static_cast<int>(w32(S + 0xA2C)), static_cast<int>(w32(S + 0xA30)),
                    w32(S + 0xA34), static_cast<int16_t>(w16(S + 0xA38)), w32(0x80075668u), w32(0x800A83A0u));
        std::printf("[PSRNX]   sched queue 800A6CD0: valid=%u first=%u count=%u\n", w32(0x800A6CD0u + 8), w32(0x800A6CD0u + 12),
                    w32(0x800A6CD0u + 16));
        struct Task { const char* name; uint32_t addr; };
        const Task tasks[] = {{"A14", w32(S + 0xA14)}, {"A18", w32(S + 0xA18)}, {"A1C", w32(S + 0xA1C)}, {"A20", w32(S + 0xA20)},
                              {"A24", w32(S + 0xA24)}, {"gfxtask D_800846C0", 0x800846C0u},
                              {"D_800A83A8[0]", 0x800A83A8u}, {"D_800A83A8[1]", 0x800A8410u}};
        for (const Task& t : tasks) {
            if (t.addr == 0) continue;
            std::printf("[PSRNX]   task %-18s %08X: type=%u unk_1C=%u unk_1E=%u done-queue valid=%u\n", t.name, t.addr,
                        w32(t.addr + 0x20), w16(t.addr + 0x1C), w16(t.addr + 0x1E), w32(t.addr + 4 + 8));
        }
    }

    // The last scheduler-class events (task start/complete, scheduler messages), oldest first.
    void dump_sched_events(uint32_t n) {
        const uint64_t count = recomp_ultra_trace_sched_count();
        const uint64_t cap = recomp_ultra_trace_sched_capacity();
        const uint64_t first = count > n ? count - n : 0;
        std::printf("[PSRNX] scheduler events: %llu total, last %llu:\n", static_cast<unsigned long long>(count),
                    static_cast<unsigned long long>(count - first));
        (void)cap;
        recomp_ultra_trace_event ev{};
        for (uint64_t i = first; i < count; ++i) {
            if (!recomp_ultra_trace_sched_get(i, &ev)) continue;
            std::printf("[PSRNX]   e#%llu +%llums t%u %s a0=%08X a1=%08X a2=%08X a3=%08X\n", static_cast<unsigned long long>(i),
                        static_cast<unsigned long long>(ev.ms), static_cast<unsigned>(ev.thread_id), ev.name, ev.a0, ev.a1, ev.a2,
                        ev.a3);
        }
    }

    // Scheduler watchdog. Stadium's scheduler thread (src/5580.c) tracks the task on the RSP in A14 and the one waiting for
    // RDP completion in A1C, and only moves on when it receives the SP-done (0x64) / DP-done (0x65) message the host posts
    // when a task finishes. If one of those messages is ever lost the slot stays occupied for good: the graphics thread waits
    // for a "DONE" that never comes while audio and retraces keep running (the freeze after a knock-out). The runtime no longer
    // drops reliable messages (n64modernruntime-extmsg.patch); this watchdog is the safety net. It only acts when the host side
    // has provably finished (every submitted task completed, nothing queued or parked) and the game state has not moved for
    // 400 ms, and then posts the missing message itself. Each heal is logged with the message-ring evidence.
    static std::atomic<uint32_t> g_watchdog_heals{0};
    void dump_lost_message_evidence() {
        const size_t cap = 65536;
        std::vector<MesgEv> ring(cap);
        size_t got = 0; uint64_t nseq = 0;
        if (ultramodern_mesg_event_size() != sizeof(MesgEv)) return;
        ultramodern_mesg_recent_copy(ring.data(), cap, &got, &nseq);
        std::printf("[PSRNX] watchdog evidence: %llu mesg events total, scanning last %zu for sched/gfx/audio queues and drops\n",
                    static_cast<unsigned long long>(nseq), got);
        size_t shown = 0;
        for (size_t i = 0; i < got && shown < 400; ++i) {
            const MesgEv& e = ring[i];
            const bool drop_class = e.op == 7 || e.op == 9 || e.op == 10;
            const bool sched_q = e.mq == 0x800A6CD0u && e.msg != 0x66u;
            const bool gfx_q = e.mq == 0x800846C4u || e.mq == 0x800A83ACu || e.mq == 0x800A8414u;
            if (!(sched_q || gfx_q || (drop_class && !(e.mq == 0x800A6CD0u && e.msg == 0x66u)))) continue;
            std::printf("[PSRNX]   m#%llu +%llums op=%u mq=%08X msg=%08X tid=%u valid %u->%u blk=%u game=%u\n",
                        static_cast<unsigned long long>(e.seq), static_cast<unsigned long long>(e.ms), e.op, e.mq, e.msg, e.tid,
                        e.vb, e.va, e.block, e.game);
            ++shown;
        }
    }

    void start_scheduler_watchdog() {
        std::thread([]() {
            constexpr uint32_t Q = 0x800A6CD0u, S = 0x800A62E0u;
            uint32_t last_a14 = 0, last_a1c = 0, stable_sp = 0, stable_dp = 0;
            while (!ultramodern::is_game_started()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                uint8_t* rdram = recomp_runtime_get_rdram();
                if (rdram == nullptr) continue;
                auto ok = [](uint32_t a) { return a >= 0x80000000u && a < 0x80800000u; };
                auto w32 = [&](uint32_t a) -> uint32_t { return ok(a) ? *reinterpret_cast<const volatile uint32_t*>(rdram + (a - 0x80000000u)) : 0xDEADBEEFu; };
                auto w16 = [&](uint32_t a) -> uint32_t { return ok(a) ? *reinterpret_cast<const volatile uint16_t*>(rdram + ((a - 0x80000000u) ^ 2u)) : 0xDEADu; };
                const uint32_t a14 = w32(S + 0xA14), a1c = w32(S + 0xA1C);
                const uint64_t submits = ultramodern_submit_gfx_count() + ultramodern_submit_audio_count() + ultramodern_submit_other_count();
                const bool host_idle = ultramodern_sp_complete_count() >= submits && !ultramodern::external_message_pending() &&
                                       ultramodern_external_parked() == 0 && w32(Q + 8) == 0;
                // SP slot: a task sits on the RSP although the host finished everything it was given.
                const bool sp_stuck = host_idle && ok(a14) && a14 != 0;
                stable_sp = (sp_stuck && a14 == last_a14) ? stable_sp + 1 : 0;
                last_a14 = sp_stuck ? a14 : 0;
                // DP slot: the current graphics task finished on the RSP (unk_1C == 3) and waits for RDP completion.
                const bool dp_stuck = host_idle && ok(a1c) && a1c != 0 && w32(a1c + 0x20) == 1 && w16(a1c + 0x1C) == 3 &&
                                      w16(a1c + 0x1E) == 1 && ultramodern_dp_complete_count() >= ultramodern_submit_gfx_count();
                stable_dp = (dp_stuck && a1c == last_a1c) ? stable_dp + 1 : 0;
                last_a1c = dp_stuck ? a1c : 0;
                if (stable_sp < 4 && stable_dp < 4) continue;
                if (g_watchdog_heals.load() >= 256) continue;
                const uint32_t n = g_watchdog_heals.fetch_add(1) + 1;
                const bool sp = stable_sp >= 4;
                std::printf("[PSRNX] WATCHDOG heal #%u: %s slot stuck (A14=%08X A1C=%08X unk_1C=%u unk_1E=%u submits=%llu sp_done=%llu"
                            " dp_done=%llu gfx_submits=%llu) -> posting scheduler message 0x%02X\n",
                            n, sp ? "RSP" : "RDP", a14, a1c, w16(a1c + 0x1C), w16(a1c + 0x1E),
                            static_cast<unsigned long long>(submits), static_cast<unsigned long long>(ultramodern_sp_complete_count()),
                            static_cast<unsigned long long>(ultramodern_dp_complete_count()),
                            static_cast<unsigned long long>(ultramodern_submit_gfx_count()), sp ? 0x64u : 0x65u);
                if (n == 1) { dump_sched_state(); dump_lost_message_evidence(); }
                ultramodern::post_rcp_event(rdram, Q, static_cast<OSMesg>(sp ? 0x64 : 0x65), false);
                stable_sp = stable_dp = 0;
                last_a14 = last_a1c = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }).detach();
    }

    // Transfer Pak diagnostics: whenever the Game Boy cartridges have seen new block I/O and then gone quiet for a second, write
    // the recent accesses (what the game read/wrote at which address, the pak's bank/enable state, which thread, when) to
    // gbcart_ring.txt next to the log. Answers "which step of the pak probe failed" after a Transfer Pak error screen.
    void start_gbcart_ring_writer() {
        std::thread([]() {
            uint32_t last = 0, settled_at = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                const uint32_t n = librecomp::gbcart::ring_count();
                if (n != last) { last = n; settled_at = 0; continue; }
                if (n == 0 || settled_at == n) continue;
                settled_at = n;
                librecomp::gbcart::ring_dump(PSRNX_DATA_DIR "/gbcart_ring.txt", 12000);
                std::printf("[PSRNX] gbcart ring: %u accesses so far, written to gbcart_ring.txt\n", n);
            }
        }).detach();
    }

    // GB Tower watcher. (1) While the embedded Game Boy emulator is alive, log one state line every 2 s (Game Boy pc, the
    // emulator's own flags, RSP signal-3 traffic, the scheduler's pending GB frame task) and write numbered snapshot files
    // (gbtower_snap_N.txt: state, Game Boy pc histogram, code around the hottest pcs, cart header, newest checkpoints) at
    // fixed times after the Game Boy CPU was initialised - i.e. DURING a stall, not after quitting. (2) Optional: if a file
    // gbtower_nudge.txt exists in the data directory, a Game Boy emulator parked for 3 s in its RSP signal wait gets signal 3
    // raised by hand (off by default: build 26 showed that does not help and it would muddy the numbers).
    void start_gbtower_watcher() {
        std::thread([]() {
            while (!ultramodern::is_game_started()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const bool nudge_enabled = std::filesystem::exists(PSRNX_DATA_DIR "/gbtower_nudge.txt");
            uint64_t last_hits = 0, last_disp = 0, last_seq = 0, last_init = 0;
            int in_wait_samples = 0, window = 0, nudges = 0;
            uint64_t window_hits_start = 0;
            int ticks_since_init = -1, next_snap = 0, last_line_tick = -100;
            static const int snap_at_s[] = {8, 20, 40, 70};
            for (int tick = 0;; ++tick) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                uint8_t* rdram = recomp_runtime_get_rdram();
                if (rdram == nullptr) continue;
                uint64_t hits = 0, misses = 0, heals = 0, heal_misses = 0, rejects = 0, runs = 0, jit = 0, jitfail = 0;
                recomp_coverage_live(&hits, &misses, &heals, &heal_misses, &rejects, &runs, &jit, &jitfail);

                const uint64_t init = psrnx_gb_init_count;
                if (init != last_init) {   // the Game Boy CPU was (re)initialised: new timeline
                    last_init = init; ticks_since_init = 0; next_snap = 0;
                    std::printf("[PSRNX] gbtower: Game Boy CPU initialised (#%llu)\n", static_cast<unsigned long long>(init));
                } else if (ticks_since_init >= 0) {
                    ++ticks_since_init;
                }
                const uint64_t disp = psrnx_gb_dispatch_count, seq = pkmnstadium_gbtower_trace_seq();
                const bool active = disp != last_disp || seq != last_seq;
                if (ticks_since_init >= 0 && ticks_since_init <= 240 && (tick - last_line_tick >= 4 || (active && tick - last_line_tick >= 4))) {
                    char line[2048];
                    psrnx_gbtower_state_line(rdram, line, sizeof(line));
                    std::printf("[PSRNX] gbtower t+%ds (+%llu opcodes) %s\n", ticks_since_init / 2,
                                static_cast<unsigned long long>(disp - last_disp), line);
                    last_line_tick = tick;
                    last_disp = disp;
                }
                last_seq = seq;
                if (ticks_since_init >= 0 && next_snap < 4 && ticks_since_init >= snap_at_s[next_snap] * 2) {
                    char path[128];
                    std::snprintf(path, sizeof(path), PSRNX_DATA_DIR "/gbtower_snap_%d.txt", next_snap + 1);
                    if (psrnx_gbtower_snapshot(rdram, path))
                        std::printf("[PSRNX] gbtower: snapshot %d written to gbtower_snap_%d.txt\n", next_snap + 1, next_snap + 1);
                    ++next_snap;
                }
                if (nudge_enabled) {
                    if (psrnx_loop_pc == 0x8120B408u) in_wait_samples++;
                    if (++window == 6) {   // 3 s window
                        const bool parked = in_wait_samples >= 5 && (hits - window_hits_start) < 2000;
                        if (parked && nudges < 400) {
                            recomp_rsp_force_sig3(rdram);
                            ++nudges;
                            if (nudges <= 12 || nudges % 50 == 0)
                                std::printf("[PSRNX] gbtower: parked in the RSP signal wait for 3 s: signal raised by hand (nudge #%d)\n", nudges);
                        }
                        window = 0; in_wait_samples = 0; window_hits_start = hits;
                    }
                }
                last_hits = hits;
            }
            (void)last_hits;
        }).detach();
    }

    void init_logging() {
        std::filesystem::create_directories(PSRNX_DATA_DIR);
        socketInitializeDefault();
        if (nxlinkStdio() >= 0) {
            setvbuf(stdout, nullptr, _IONBF, 0);
            setvbuf(stderr, nullptr, _IONBF, 0);
            return;
        }
        socketExit();
        // Launched from hbmenu: keep a log on the SD card. The previous run's log survives one launch (after a
        // crash, relaunching would otherwise erase its trace). FAT cannot rename over an existing file: remove first.
        const std::string log_path = PSRNX_DATA_DIR "/osaka-nx.log";
        const std::string previous_path = PSRNX_DATA_DIR "/osaka-nx.prev.log";
        std::remove(previous_path.c_str());
        std::rename(log_path.c_str(), previous_path.c_str());
        // "a" for both streams: with "w" on stdout its private file offset overwrote lines stderr had appended.
        freopen(log_path.c_str(), "a", stdout);
        // Unbuffered = one SD write per line, ~4 ms each on Horizon (libnx flushes every write). The runtime prints ~290 lines in a
        // row at every knock-out (candidate list of an overlay it does not know): ~1.1 s with the game stopped (build 47 logs).
        // So both streams get big buffers that a background thread empties every 50 ms (log_buffered=0 in osaka-nx.cfg = old behaviour).
        // setvbuf comes straight after each freopen: it must precede the first write to the stream.
        const bool buffered = psrnx::config().log_buffered != 0;
        static char out_buf[32 * 1024];
        static char err_buf[64 * 1024];
        if (buffered) setvbuf(stdout, out_buf, _IOFBF, sizeof(out_buf)); else setvbuf(stdout, nullptr, _IONBF, 0);
        // stderr goes to its own file: a second writable handle on the same file is refused (or its lines are lost) on
        // Horizon's file system, and the runtime and GLideN64 report through stderr. Lines from [PSRNX] use stdout.
        const std::string err_path = PSRNX_DATA_DIR "/osaka-nx.stderr.log";
        std::remove(err_path.c_str());
        const bool err_ok = freopen(err_path.c_str(), "a", stderr) != nullptr;
        if (buffered) setvbuf(stderr, err_buf, _IOFBF, sizeof(err_buf)); else setvbuf(stderr, nullptr, _IONBF, 0);
        if (!err_ok) std::printf("[PSRNX] could not open %s for stderr\n", err_path.c_str());
        if (buffered) {
            std::thread([] {
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    std::fflush(stdout);   // nothing buffered = no write
                    std::fflush(stderr);
                }
            }).detach();
            std::printf("[PSRNX] log files are buffered, flushed every 50 ms (osaka-nx.cfg log_buffered=0 = one write per line)\n");
        }
    }

    // The runtime writes hash_miss_id_EF.bin (50 KB through a ~1 KB stdio buffer) at every overlay it cannot identify - every
    // knock-out. A folder with that name makes its fopen fail, which skips the dump. A file left by an earlier run is kept as .sample.
    void block_hash_miss_dump() {
        if (psrnx::config().hash_miss_dump) return;
        std::error_code ec;
        const std::string path = PSRNX_DATA_DIR "/hash_miss_id_EF.bin";
        if (std::filesystem::is_directory(path, ec)) return;
        if (std::filesystem::exists(path, ec)) {
            const std::string kept = PSRNX_DATA_DIR "/hash_miss_id_EF.sample.bin";
            std::remove(kept.c_str());   // FAT cannot rename over an existing file
            std::rename(path.c_str(), kept.c_str());
        }
        std::filesystem::create_directories(path, ec);
        if (ec) std::printf("[PSRNX] could not turn hash_miss_id_EF.bin into a folder: %s\n", ec.message().c_str());
    }

    bool is_rom_extension(std::string ext) {
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext == ".z64" || ext == ".n64" || ext == ".v64";
    }

    // librecomp validates by hash (US v1.0, MD5 ed1378bc12115f71209a77844965ba50) and keeps the byteswapped copy.
    bool select_rom(std::u8string& game_id) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(PSRNX_DATA_DIR, ec)) {
            if (!entry.is_regular_file() || !is_rom_extension(entry.path().extension().string())) continue;
            const auto result = recomp::select_rom(entry.path(), game_id);
            std::printf("[PSRNX] ROM %s -> validation %d\n", entry.path().c_str(), static_cast<int>(result));
            if (result == recomp::RomValidationError::Good) return true;
        }
        ultramodern::error_handling::message_box(
            "No valid Pokemon Stadium ROM found.\n"
            "Copy your US v1.0 dump (.z64/.n64/.v64, MD5 ed1378bc12115f71209a77844965ba50) to "
            "sdmc:/switch/osaka-nx/");
        return false;
    }

    void set_switch_graphics_config() {
        ultramodern::renderer::GraphicsConfig config{};
        config.developer_mode = false;
        config.res_option = ultramodern::renderer::Resolution::Original;
        config.wm_option = ultramodern::renderer::WindowMode::Fullscreen;
        config.hr_option = ultramodern::renderer::HUDRatioMode::Original;
        config.api_option = ultramodern::renderer::GraphicsApi::Auto;
        config.ar_option = ultramodern::renderer::AspectRatio::Original;
        config.msaa_option = ultramodern::renderer::Antialiasing::None;
        config.rr_option = ultramodern::renderer::RefreshRate::Original;
        config.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Off;
        config.ds_option = 1;
        ultramodern::renderer::set_graphics_config(config);
    }

    ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != 0) {
            std::fprintf(stderr, "[PSRNX] SDL_Init failed: %s\n", SDL_GetError());
        }
        return {};
    }

    ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
        step("create window");
        psrnx_window_set_size(psrnx::screen_width(), psrnx::screen_height());
        psrnx_window = SDL_CreateWindow("Osaka-NX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        psrnx::screen_width(), psrnx::screen_height(), SDL_WINDOW_OPENGL);
        if (psrnx_window == nullptr) {
            std::fprintf(stderr, "[PSRNX] SDL_CreateWindow failed: %s\n", SDL_GetError());
        }
        step("window created");
        return ultramodern::renderer::WindowHandle{psrnx_window};
    }

    void update_gfx(void*) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                // As the PC runner: stop, let the saving thread flush FlashRAM (Stadium's registered teams are
                // written by a background thread that coalesces writes for up to ~1.3 s), then leave. The full
                // join sequence in recomp::start can hang, and the Switch has nothing to gain from it.
                ultramodern::quit();
                ultramodern::join_saving_thread();
                psrnx_close_audio();
                std::fflush(nullptr);
                svcExitProcess();
            }
            psrnx_input_handle_event(event);
        }
    }

    std::string get_game_thread_name(const OSThread*) {
        return "PokemonStadium";
    }

    void error_message_box(const char* message) {
        std::fprintf(stderr, "[PSRNX ERROR] %s\n", message);
    }
}

namespace {
    // Result of the one-time data-folder move (data_migration.h), printed once the log file is open.
    std::string g_migration_note;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    std::set_terminate(log_terminate);
    // Must come first: the picker and the settings reader already use the new folder name.
    g_migration_note = psrnx_migrate::migrate_data_folder("sdmc:/switch/pokestadiumrecomp", PSRNX_DATA_DIR,
                                                          "/switch/pokestadiumrecomp/", "/switch/osaka-nx/");
    // Before init_logging: the picker owns the console (stdout) until it is done.
    std::string picker_log;
    if (!psrnx_run_gb_picker(picker_log)) return EXIT_SUCCESS;
    init_logging();
    std::printf("[PSRNX] %s\n", picker_log.c_str());
    if (!g_migration_note.empty()) std::printf("[PSRNX] data folder: %s\n", g_migration_note.c_str());
    psrnx_place_thread("Main");   // register the main thread with the stall sampler (it can own locks other threads wait on)
    std::printf("[PSRNX] Osaka-NX (Switch), applet type %d (%llu ms)\n",
                static_cast<int>(appletGetAppletType()), psrnx_ms_since_boot());

    // No libtcc on Horizon (it is dlopen-ed, and dlopen is a stub here): the overlay/fragment JIT tiers must be off
    // rather than probing for it. A function miss then falls to the portable MIPS interpreter tier, or aborts.
    setenv("PSR_JIT_TIER", "0", 1);
    setenv("PSR_FRAG_JIT", "0", 1);
    setenv("PSR_FRAG_NATIVE", "0", 1);

    step("audio init");
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!psrnx_reset_audio(48000)) {
        return EXIT_FAILURE;
    }

    step("register game");
    recomp::Version project_version{0, 1, 0, ""};
    recomp::register_config_path(PSRNX_DATA_DIR);
    // The runtime writes last_error.log and coverage/runtime_captures.json relative to the working directory.
    if (chdir(PSRNX_DATA_DIR) != 0) std::printf("[PSRNX] chdir(%s) failed\n", PSRNX_DATA_DIR);
    if (psrnx::config().interp_discovery) setenv("PSR_INTERP_DISCOVERY", "1", 1);
    block_hash_miss_dump();
    // Mesa reads this when it translates the first shader (nvc0_program_translate): set it before the window and GL context exist.
    if (psrnx::config().shader_opt >= 0) {
        char level[8];
        std::snprintf(level, sizeof(level), "%d", psrnx::config().shader_opt);
        setenv("NV50_PROG_OPTIMIZE", level, 1);
        std::printf("[PSRNX] nouveau shader optimisation level %s (NV50_PROG_OPTIMIZE; osaka-nx.cfg shader_opt=-1 keeps Mesa's default 3)\n", level);
    }
    pkmnstadium::transfer_pak::initialize();
    for (int port = 0; port < 4; ++port) {
        std::printf("[PSRNX] transfer pak port %d: %s\n", port + 1,
                    pkmnstadium::transfer_pak::has_transfer_pak(port) ? "GB cart mounted" : "none");
    }
    if (!std::filesystem::exists(PSRNX_DATA_DIR "/launcher.cfg")) {
        std::printf("[PSRNX] no launcher.cfg in %s (copy launcher.cfg.example there to use a Transfer Pak / GB Tower)\n",
                    PSRNX_DATA_DIR);
    }
    set_switch_graphics_config();
    std::u8string game_id = psrnx_register_stadium();

    step("rom check");
    if (!select_rom(game_id)) {
        return EXIT_FAILURE;
    }

    // start_game must wait until recomp::start has spun up the VI thread and installed the dummy VI mode (the
    // game races to osViSetMode, and update_vi dereferences a null mode otherwise). Same 100 ms grace as the PC runner.
    std::thread([game_id]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        recomp::start_game(game_id);
        std::fprintf(stderr, "[PSRNX] start_game fired\n");
    }).detach();

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = psrnx_get_rsp_microcode,
    };
    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = psrnx_create_render_context,
    };
    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = psrnx_queue_samples,
        .get_frames_remaining = psrnx_get_frames_remaining,
        .set_frequency = psrnx_set_frequency,
    };
    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = psrnx_poll_input,
        .get_input = psrnx_get_input,
        .set_rumble = psrnx_set_rumble,
        .get_connected_device_info = psrnx_get_connected_device_info,
    };
    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };
    ultramodern::events::callbacks_t events_callbacks{
        .vi_callback = []() {
            psrnx_diag::vi_ticks.fetch_add(1, std::memory_order_relaxed);
            psrnx_update_rumble();
        },
    };
    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = error_message_box,
    };
    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = get_game_thread_name,
    };

    // Heartbeat: every 2 s for the first minute, then every 10 s. Shows whether the game, the VI thread, the graphics
    // path and the audio path are alive, and (by comparing lines) which one stopped.
    start_scheduler_watchdog();
    // Off unless osaka-nx.cfg says gbcart_ring=1: librecomp::gbcart::ring_dump holds the pak mutex while it writes the file, which froze
    // the game for 0.9 ms per entry (4-11 s per knock-out in the build 46 run).
    if (psrnx::config().gbcart_ring) start_gbcart_ring_writer();
    else std::printf("[PSRNX] gbcart ring writer off (osaka-nx.cfg gbcart_ring=1 turns it on; it freezes the game while writing)\n");
    start_gbtower_watcher();
    std::thread([]() {
        uint64_t last_vi = 0, last_dl = 0, last_ns_dl = 0, last_ns_scr = 0, last_interp_ns = 0;
        int stalled_beats = 0;
        for (int i = 0;; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(i < 30 ? 2 : 10));
            const uint64_t vi = psrnx_diag::vi_ticks.load();
            std::printf("[PSRNX] hb t=%llu ms started=%d vi=%llu (+%llu) send_dl=%llu screen=%llu audio=%llu bufs/%llu frames"
                        " | submit gfx=%llu aud=%llu other=%llu sp_done=%llu dp_done=%llu\n",
                        psrnx_ms_since_boot(), static_cast<int>(ultramodern::is_game_started()),
                        static_cast<unsigned long long>(vi), static_cast<unsigned long long>(vi - last_vi),
                        static_cast<unsigned long long>(psrnx_diag::send_dl.load()),
                        static_cast<unsigned long long>(psrnx_diag::update_screen.load()),
                        static_cast<unsigned long long>(psrnx_diag::audio_pushes.load()),
                        static_cast<unsigned long long>(psrnx_diag::audio_frames.load()),
                        static_cast<unsigned long long>(ultramodern_submit_gfx_count()),
                        static_cast<unsigned long long>(ultramodern_submit_audio_count()),
                        static_cast<unsigned long long>(ultramodern_submit_other_count()),
                        static_cast<unsigned long long>(ultramodern_sp_complete_count()),
                        static_cast<unsigned long long>(ultramodern_dp_complete_count()));
            const uint64_t dl_ns = psrnx_diag::send_dl_ns.load(), scr_ns = psrnx_diag::update_screen_ns.load();
            std::printf("[PSRNX]   gpu time since last beat: ProcessDList %llu ms, UpdateScreen %llu ms, slow calls so far %llu\n",
                        static_cast<unsigned long long>((dl_ns - last_ns_dl) / 1000000),
                        static_cast<unsigned long long>((scr_ns - last_ns_scr) / 1000000),
                        static_cast<unsigned long long>(psrnx_diag::slow_calls.load()));
            last_ns_dl = dl_ns; last_ns_scr = scr_ns;
            std::printf("[PSRNX]   host->guest messages: parked_now=%u parked_total=%llu watchdog_heals=%u\n",
                        ultramodern_external_parked(), static_cast<unsigned long long>(ultramodern_external_requeues()),
                        g_watchdog_heals.load());
            {
                static uint64_t last_loop_ticks = 0;
                const uint64_t lt = psrnx_loop_ticks;
                std::printf("[PSRNX]   loops: back-edges=%llu (+%llu) last pc=0x%08X (hook: wrapped)\n",
                            static_cast<unsigned long long>(lt), static_cast<unsigned long long>(lt - last_loop_ticks),
                            static_cast<unsigned>(psrnx_loop_pc));
                last_loop_ticks = lt;
                {
                    uint64_t created = 0, reaped = 0, tracked = 0;
                    psrnx_thread_stats(&created, &reaped, &tracked);
                    std::printf("[PSRNX]   threads: created=%llu reaped=%llu live-or-unreaped=%llu\n", static_cast<unsigned long long>(created),
                                static_cast<unsigned long long>(reaped), static_cast<unsigned long long>(tracked));
                }
            }
            // Code the static recompile has no function for runs in the MIPS interpreter (slow). misses = calls through
            // the function table that found nothing, interp = entries into the interpreter, interp_ms = wall time in it.
            {
                uint64_t hits = 0, misses = 0, heals = 0, heal_misses = 0, rejects = 0, runs = 0, jit = 0, jitfail = 0;
                recomp_coverage_live(&hits, &misses, &heals, &heal_misses, &rejects, &runs, &jit, &jitfail);
                uint64_t interp_ns = 0, interp_entries = 0;
                recomp_interp_stats(&interp_ns, &interp_entries);
                std::printf("[PSRNX]   tiers: static_hits=%llu misses=%llu self_heals=%llu interp_runs=%llu entries=%llu"
                            " interp_ms(+%llu)=%llu\n",
                            static_cast<unsigned long long>(hits), static_cast<unsigned long long>(misses),
                            static_cast<unsigned long long>(heals), static_cast<unsigned long long>(runs),
                            static_cast<unsigned long long>(interp_entries),
                            static_cast<unsigned long long>((interp_ns - last_interp_ns) / 1000000),
                            static_cast<unsigned long long>(interp_ns / 1000000));
                last_interp_ns = interp_ns;
            }
            // Stall detector: no display list for two beats in a row after start-up -> dump every thread once, and
            // again after another four beats if it is still stuck.
            const uint64_t dl = psrnx_diag::send_dl.load();
            stalled_beats = (i > 6 && dl == last_dl) ? stalled_beats + 1 : 0;
            last_dl = dl;
            if (stalled_beats == 2 || stalled_beats == 6) {
                std::printf("[PSRNX] STALL: no display list for %d beats\n", stalled_beats);
                dump_threads();
                dump_sched_state();
                dump_sched_events(120);
                if (stalled_beats == 2) { dump_trace("stall", 20000, 80); dump_rings(70);
                    static const uint32_t interesting[] = {0x800846C0, 0x8008468C, 0x80083BEC, 0x80083E60, 0x800A6CD0, 0x800A8640};
                    dump_qstates(interesting, 6); }
            }
            last_vi = vi;
            if (i == 5) {            // about 12 s in
                dump_threads();
                dump_trace("12s", 4000, 0);
            } else if (i == 20) {    // about 42 s in
                dump_threads();
            }
        }
    }).detach();


    // Stall sampler: when no display list has started for a second, look at where every game thread is executing every
    // 50 ms, and print a histogram per thread. Addresses are runtime addresses; switch/symbolize.sh turns them into
    // function names with the ELF. Also requests the OS CPU boost clock while the game computes without drawing.
    std::thread([]() {
        struct Sample { uint64_t pc, lr; };
        constexpr int MAX_THREADS = 32, MAX_SAMPLES = 160;
        static Sample samples[MAX_THREADS][MAX_SAMPLES];
        static int counts[MAX_THREADS];
        static PsrnxSample last_ex[MAX_THREADS];
        static bool have_ex[MAX_THREADS];
        static uint32_t loop_samples[MAX_SAMPLES];
        static int loop_count = 0;
        static uint64_t loop_ticks_at_start = 0;
        std::printf("[PSRNX] prof: marker psrnx_code_marker=0x%llx\n", static_cast<unsigned long long>(psrnx_code_marker()));
        const auto summary = [](const char* why, int stall_ms) {
            std::printf("[PSRNX] prof %s: stalled %d ms, %d threads\n", why, stall_ms, psrnx_thread_count());
            if (loop_count > 0) {
                uint32_t lpcs[MAX_SAMPLES]; int lhits[MAX_SAMPLES], ld = 0;
                for (int i = 0; i < loop_count; ++i) {
                    int k = 0;
                    while (k < ld && lpcs[k] != loop_samples[i]) ++k;
                    if (k == ld) { lpcs[ld] = loop_samples[i]; lhits[ld++] = 0; }
                    ++lhits[k];
                }
                std::printf("[PSRNX] prof   loop back-edges since stall start: %llu; guest loop pcs:",
                            static_cast<unsigned long long>(psrnx_loop_ticks - loop_ticks_at_start));
                for (int shown = 0; shown < 6 && shown < ld; ++shown) {
                    int best = shown;
                    for (int k = shown + 1; k < ld; ++k) if (lhits[k] > lhits[best]) best = k;
                    std::swap(lpcs[shown], lpcs[best]); std::swap(lhits[shown], lhits[best]);
                    std::printf(" 0x%08X x%d", lpcs[shown], lhits[shown]);
                }
                std::printf("\n");
            }
            for (int t = 0; t < psrnx_thread_count() && t < MAX_THREADS; ++t) {
                if (counts[t] == 0) continue;
                // distinct PCs with counts, top 5
                uint64_t pcs[MAX_SAMPLES], lrs[MAX_SAMPLES];
                int hits[MAX_SAMPLES], distinct = 0;
                for (int i = 0; i < counts[t]; ++i) {
                    int k = 0;
                    while (k < distinct && pcs[k] != samples[t][i].pc) ++k;
                    if (k == distinct) { pcs[distinct] = samples[t][i].pc; lrs[distinct] = samples[t][i].lr; hits[distinct++] = 0; }
                    ++hits[k];
                }
                std::printf("[PSRNX] prof   %-14s n=%d distinct=%d:", psrnx_thread_name(t), counts[t], distinct);
                for (int shown = 0; shown < 5 && shown < distinct; ++shown) {
                    int best = shown;
                    for (int k = shown + 1; k < distinct; ++k) if (hits[k] > hits[best]) best = k;
                    std::swap(pcs[shown], pcs[best]); std::swap(lrs[shown], lrs[best]); std::swap(hits[shown], hits[best]);
                    std::printf(" pc=0x%llx(lr=0x%llx)x%d", static_cast<unsigned long long>(pcs[shown]),
                                static_cast<unsigned long long>(lrs[shown]), hits[shown]);
                }
                std::printf("\n");
                if (have_ex[t]) {
                    const PsrnxSample& e = last_ex[t];
                    std::printf("[PSRNX] prof     last non-idle: pc=0x%llx lr=0x%llx bt:", (unsigned long long)e.pc, (unsigned long long)e.lr);
                    for (int k = 0; k < e.nbt; ++k) std::printf(" 0x%llx", (unsigned long long)e.bt[k]);
                    std::printf("\n");
                    if (e.blocked_on_mutex) {
                        std::printf("[PSRNX] prof     BLOCKED ON MUTEX 0x%llx owned by handle 0x%x (%s): owner pc=0x%llx lr=0x%llx bt:",
                                    (unsigned long long)e.mutex_addr, e.owner_handle, e.owner_name,
                                    (unsigned long long)e.owner_pc, (unsigned long long)e.owner_lr);
                        for (int k = 0; k < e.owner_nbt; ++k) std::printf(" 0x%llx", (unsigned long long)e.owner_bt[k]);
                        std::printf("\n");
                    }
                }
            }
        };
        uint64_t last_dl = 0;
        int idle_ms = 0, rounds = 0;
        bool active = false, boosted = false, boost_reported = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const uint64_t dl = psrnx_diag::send_dl.load();
            if (dl != last_dl) {
                last_dl = dl;
                if (active) {
                    summary("end", idle_ms);
                    active = false;
                    std::memset(counts, 0, sizeof(counts));
                    std::memset(have_ex, 0, sizeof(have_ex));
                    loop_count = 0;
                    rounds = 0;
                }
                if (boosted) { appletSetCpuBoostMode(ApmCpuBoostMode_Normal); boosted = false; }
                idle_ms = 0;
                continue;
            }
            idle_ms += 50;
            if (idle_ms < 1000) continue;
            if (!active) loop_ticks_at_start = psrnx_loop_ticks;
            active = true;
            if (loop_count < MAX_SAMPLES) loop_samples[loop_count++] = psrnx_loop_pc;
            // FastLoad also throttles the GPU: only while the gfx thread is not inside a display list.
            const bool want_boost = psrnx::config().stall_boost && !psrnx_diag::in_process_dl.load(std::memory_order_relaxed);
            if (want_boost != boosted) {
                const Result rc = appletSetCpuBoostMode(want_boost ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
                if (!boost_reported) { std::printf("[PSRNX] cpu boost request: rc=0x%x\n", rc); boost_reported = true; }
                if (R_SUCCEEDED(rc)) boosted = want_boost;
            }
            for (int t = 0; t < psrnx_thread_count() && t < MAX_THREADS; ++t) {
                if (counts[t] >= MAX_SAMPLES) continue;
                PsrnxSample e;
                if (psrnx_sample_thread_ex(t, &e)) {
                    samples[t][counts[t]++] = {e.pc, e.lr};
                    if (!e.idle) {   // keep the newest busy sample; a mutex wait is worth more than a plain one
                        if (!have_ex[t] || e.blocked_on_mutex || !last_ex[t].blocked_on_mutex) { last_ex[t] = e; have_ex[t] = true; }
                    }
                }
            }
            if (++rounds % 100 == 0) summary("mid", idle_ms);   // every 5 s of one long stall
        }
    }).detach();

    step("recomp::start");
    recomp::start(project_version, {}, rsp_callbacks, renderer_callbacks, audio_callbacks, input_callbacks,
                  gfx_callbacks, events_callbacks, error_handling_callbacks, threads_callbacks);
    step("recomp::start returned");

    psrnx_close_audio();
    std::fflush(nullptr);
    // Game threads may still be running: end the process rather than return into the homebrew loader (see README,
    // "Exit"). This returns to the Home menu, not to hbmenu.
    svcExitProcess();
    return EXIT_SUCCESS;
}
