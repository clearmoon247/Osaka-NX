# Patches to third-party code

Osaka-NX compiles two outside projects from source. It does not copy them into this repository: `switch/setup_deps.sh`
downloads each one at a pinned revision and applies one patch file to it.

| Patch file | Applied to | Pinned revision |
| --- | --- | --- |
| `n64modernruntime.patch` | [mstan/N64ModernRuntime](https://github.com/mstan/N64ModernRuntime) (the runtime the recompiled game calls) | `d4bf882` |
| `gliden64.patch` | [gonetz/GLideN64](https://github.com/gonetz/GLideN64) (the OpenGL renderer) | `41c7ba273a6c9afb43c0574cf3cf5d139182d070` |

A patch is a plain-text list of the exact lines added and removed (`git diff` format), so the whole of what we change in
these projects can be read in those two files. This page explains each part. If a hunk in a patch is not listed here, that
is a documentation bug: please report it.

To check a patch yourself, from the repository root:

```
git -C lib/GLideN64 diff --stat      # after setup_deps.sh: shows the four files changed in GLideN64
git -C lib/N64ModernRuntime diff --stat
```

Most additions are written so that they do nothing on other platforms: Switch-only code sits behind `#ifdef __SWITCH__`, and
the diagnostics cost one pointer or flag test when nothing uses them.

## n64modernruntime.patch

The runtime implements the N64 operating system calls (threads, message queues, the scheduler, the RSP, the Transfer Pak)
for the recompiled game. Seventeen files change. They fall into five groups.

### 1. Making it build and run on Horizon (the Switch's operating system)

| File | What changes | Why |
| --- | --- | --- |
| `librecomp/include/librecomp/addresses.hpp` | On Switch the guest memory window is 1 GiB instead of 4 GiB. | Horizon has no `mmap`/`mprotect`, so the window is one block that is committed up front (`switch/mman_switch.c`). |
| `librecomp/include/librecomp/audio_uaf_protect.hpp` | Adds `#include <cstddef>` and `<stddef.h>`. | The header uses `size_t` but includes only `<cstdint>`; the Switch toolchain's C library does not define it from there. |
| `thirdparty/concurrentqueue/lightweightsemaphore.h` | Treats `__SWITCH__` like `__unix__` (two `#elif` lines). | Selects the POSIX semaphore implementation, which libnx provides. |
| `ultramodern/include/ultramodern/renderer_context.hpp` | Treats `__SWITCH__` like Linux: the window handle is an `SDL_Window*`. | One `#elif` line. |
| `ultramodern/src/error_handling.cpp` | Exits with `_Exit` instead of `quick_exit` on Switch (the branch macOS already takes). | One `#if` line; `quick_exit` is not used on this platform. |
| `ultramodern/src/threads.cpp` | On Switch, thread names and priorities call `psrnx_place_thread` / `psrnx_set_thread_priority` (`switch/threads_switch.c`). | A thread's name decides which CPU core it runs on; priorities map to Horizon priorities. |
| `librecomp/src/files.cpp` | Save-file backup rotation uses renames, not `copy_file`. | The standard library's `copy_file` ends with `fchmod`, which libnx's SD card driver rejects, so every copy failed. FAT will not rename onto an existing file, hence the explicit removes. |

### 2. Message delivery and screen updates (fixes for stalls and lost events)

| File | What changes | Why |
| --- | --- | --- |
| `ultramodern/src/mesgqueue.cpp` | Events sent from the host to the game (scheduler ticks, retrace, task completion) are kept and retried in order when the game's queue is full, instead of being dropped. A counter of parked messages is kept for the log. | A dropped "task finished" message made the game wait forever. |
| `ultramodern/src/events.cpp` | Screen updates the graphics thread has not taken yet are coalesced: an update that has a newer one behind it is skipped. | After one slow display list (the first shader compiles) a backlog built up that never shrank at the video rate, and the game starved. |

### 3. Recompiled-code coverage

| File | What changes | Why |
| --- | --- | --- |
| `librecomp/src/overlays.cpp` | The game's shared fragment slot (id `0xEF`) can hold several live fragments at once. A fragment loaded there is now retired only when a newer load overwrites its memory, instead of whenever any new one is loaded. Setting the environment variable `PSR_FRAG_MULTI=0` restores the old behaviour. | The asset loader decompresses many different fragments into that slot (for a battle: stage, move effect and model code). Retiring the earlier ones made their next call miss the function table, and the battle froze after a knock-out. |
| `librecomp/src/mips_interp.cpp` | Measures the time spent in the MIPS interpreter (outermost entry only). | Diagnostic for the log heartbeat; the interpreter is the slow path for code the static recompile did not contain. |

### 4. Game Boy Tower / Transfer Pak

| File | What changes | Why |
| --- | --- | --- |
| `librecomp/include/librecomp/gbcart.hpp`, `librecomp/src/gbcart.cpp`, `librecomp/src/gbpak.cpp` | A small ring buffer records Transfer Pak block reads and writes, and the first 200 failed calls are logged. A cartridge's open-bus area starts at the next 16 KiB bank boundary after the last programmed byte; the last bank that holds data keeps its zero tail as dumped. | The ring is a diagnostic (off unless `gbcart_ring=1`). The bank rule matches what GB Tower checks (it verifies that bank against a checksum table). |
| `librecomp/src/rsp.cpp` | A helper raises RSP status signal 3 by hand when the Game Boy emulator's poll for it has made no progress for seconds; counters note writes to signals 2 and 3. | GB Tower's emulator waits for that signal to learn that its RSP task started; a lost handshake otherwise holds it forever. |

### 5. Scheduler trace

| File | What changes | Why |
| --- | --- | --- |
| `ultramodern/include/ultramodern/ultra_trace.hpp`, `ultramodern/src/ultra_trace.cpp` | A small ring that records only scheduler-class events (RSP/RDP task start and completion and the messages to the game's scheduler queue). | Diagnostic: it survives long enough to see what a scheduler stall looked like. It is read by the stall report in `switch/main_switch.cpp`. |

## gliden64.patch

GLideN64 is the renderer. Four files change; all additions are small, and each is described by a comment in the patch.

| File | What changes | Why |
| --- | --- | --- |
| `src/Graphics/OpenGLContext/ThreadedOpenGl/atomicops.h` | Treats `__SWITCH__` like `__unix__` in two `#elif` lines. | Selects the POSIX semaphore implementation, which libnx provides. |
| `src/FrameBuffer.cpp`, `src/VI.cpp` | A hook pointer (`psrnx_present_hook`) is called with how the frame was composed, just before it is swapped to the screen. The port uses it to hide garbage frames after a video-mode change, to crop and stretch the Game Boy Tower picture, and to record the presented pictures. A setting (`psrnx_vi_field_mode`) picks which field of Stadium's interlaced menus is shown. | Without the field setting GLideN64 flipped the field every frame, which moved the picture one line up and down. |
| `src/Combiner.cpp` | Shader start-up warm-up and shader statistics. **Warm-up:** before the game starts, the shader programs a list says the game needs are built (the list is kept by `switch/render_gliden64.cpp`, within a time budget set by `shader_warm_ms`). **Build timing:** every synchronous shader build is timed. **Used keys:** the shader keys the game really draws with are reported, so later starts build only those. | The Switch's graphics driver takes 100-500 ms to build one shader and has no cache, so each effect drawn for the first time froze the game. |

Names beginning `psrnx_` are the project's internal identifiers (the old working name). They are not user-visible; the only
exceptions are log lines that start with `[PSRNX]`.
