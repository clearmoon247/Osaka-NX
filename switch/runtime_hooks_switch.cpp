// Symbols that mstan's N64ModernRuntime fork expects the application to provide. On PC they live in the Windows
// runner's debug server and post-mortem writer (src/main/debug_server.cpp, post_mortem.cpp), which this port does not
// build. The runtime only reads the counters for its own trace rings and calls the dump on fatal paths.
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace pkmnstadium { namespace dbg {
    std::atomic<uint64_t> g_frame_count{0};
    std::atomic<uint64_t> g_send_dl_count{0};
    std::atomic<uint64_t> g_send_dl_gfx_count{0};
}}

// Called by librecomp (unhandled function lookup) and ultramodern (send to an invalid message queue) just before the
// process gives up. The reason string is the useful part; it goes to the log (sdmc:/switch/osaka-nx/).
extern "C" void psr_post_mortem_dump(const char* reason, void* /*fault_info*/) {
    std::fprintf(stderr, "[post-mortem] %s\n", reason ? reason : "(null)");
    std::fflush(stderr);
}
