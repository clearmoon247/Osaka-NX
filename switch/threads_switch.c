// Threading fixes for libnx, which the runtime's std::thread usage runs into:
// - 128 KiB default stacks (newlib.c, __syscall_thread_create) vs 8 MiB on Linux, 512 KiB on macOS;
//   recompiled N64 code uses large frames. std::thread cannot pass attributes, so it is linked with
//   -Wl,--wrap=pthread_create,--wrap=pthread_detach and lands here.
// - pthread_detach is unsupported (ENOSYS), which made std::thread::detach throw.
// - every thread starts on core 0: ultramodern's thread names/priorities are honoured below.
#include <errno.h>
#include <pthread.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "psrnx_sample.h"

// Twice what the macOS build runs with.
#define PSRNX_THREAD_STACK_SIZE (1024 * 1024)

int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);
int __real_pthread_join(pthread_t thread, void** value);

// Detached threads must still be joined on Horizon. libnx implements neither pthread_detach (ENOSYS) nor thread
// self-cleanup: an exited thread keeps its kernel thread object and its stack until somebody joins it. The runtime detaches
// a thread per call in places (the Transfer Pak spawns two per 32-byte block access, librecomp gbpak.cpp), and leaking
// them ran the process out of threads/memory ("Not enough space" = ENOMEM from pthread_create a few seconds after boot
// with a cartridge mounted). So every thread runs through a trampoline that records when it has finished, pthread_detach
// only marks it, and finished detached threads are joined (reaped) whenever another thread is created or detached.
typedef struct Tramp {
    struct Tramp* next;
    pthread_t th;
    void* (*start)(void*);
    void* arg;
    volatile int finished;
    volatile int detached;
} Tramp;

static Mutex g_tramp_mutex;                 // zero-initialised is a valid libnx mutex
static Tramp* g_tramps;                     // every created thread not yet joined/reaped
static volatile uint64_t g_threads_created, g_threads_reaped;

static void* tramp_entry(void* p) {
    Tramp* t = (Tramp*)p;
    void* result = t->start(t->arg);
    __atomic_store_n(&t->finished, 1, __ATOMIC_RELEASE);
    return result;
}

static void reap_detached(void) {
    for (;;) {
        Tramp* victim = NULL;
        mutexLock(&g_tramp_mutex);
        for (Tramp** pp = &g_tramps; *pp != NULL; pp = &(*pp)->next) {
            Tramp* t = *pp;
            if (__atomic_load_n(&t->detached, __ATOMIC_ACQUIRE) && __atomic_load_n(&t->finished, __ATOMIC_ACQUIRE)) {
                *pp = t->next;
                victim = t;
                break;
            }
        }
        mutexUnlock(&g_tramp_mutex);
        if (victim == NULL) return;
        __real_pthread_join(victim->th, NULL);   // the thread is at its last instructions: this returns at once
        __atomic_fetch_add(&g_threads_reaped, 1, __ATOMIC_RELAXED);
        free(victim);
    }
}

void psrnx_thread_stats(uint64_t* created, uint64_t* reaped, uint64_t* tracked) {
    uint64_t n = 0;
    mutexLock(&g_tramp_mutex);
    for (Tramp* t = g_tramps; t != NULL; t = t->next) n++;
    mutexUnlock(&g_tramp_mutex);
    *created = g_threads_created;
    *reaped = g_threads_reaped;
    *tracked = n;
}

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    reap_detached();
    Tramp* t = (Tramp*)calloc(1, sizeof(Tramp));
    if (t == NULL) return ENOMEM;
    t->start = start;
    t->arg = arg;

    pthread_attr_t sized;
    const pthread_attr_t* use = attr;
    if (attr == NULL) {
        pthread_attr_init(&sized);
        pthread_attr_setstacksize(&sized, PSRNX_THREAD_STACK_SIZE);
        use = &sized;
    }
    int result = __real_pthread_create(thread, use, tramp_entry, t);
    if (result != 0) {   // out of threads or memory: finished detached threads may still hold both, so retry once
        reap_detached();
        result = __real_pthread_create(thread, use, tramp_entry, t);
    }
    if (attr == NULL) pthread_attr_destroy(&sized);
    if (result != 0) {
        free(t);
        return result;
    }
    t->th = *thread;
    __atomic_fetch_add(&g_threads_created, 1, __ATOMIC_RELAXED);
    mutexLock(&g_tramp_mutex);
    t->next = g_tramps;
    g_tramps = t;
    mutexUnlock(&g_tramp_mutex);
    return 0;
}

int __wrap_pthread_join(pthread_t thread, void** value) {
    const int result = __real_pthread_join(thread, value);
    if (result == 0) {   // joined by its owner: forget the record
        Tramp* gone = NULL;
        mutexLock(&g_tramp_mutex);
        for (Tramp** pp = &g_tramps; *pp != NULL; pp = &(*pp)->next) {
            if (pthread_equal((*pp)->th, thread)) {
                gone = *pp;
                *pp = gone->next;
                break;
            }
        }
        mutexUnlock(&g_tramp_mutex);
        free(gone);
    }
    return result;
}

// libnx leaves __syscall_thread_detach unimplemented ("Unsupported" in newlib.c), so pthread_detach returns ENOSYS and
// std::thread::detach throws (ultramodern's timer thread). The thread is marked here and joined by reap_detached once it
// has finished.
int __wrap_pthread_detach(pthread_t thread) {
    mutexLock(&g_tramp_mutex);
    for (Tramp* t = g_tramps; t != NULL; t = t->next) {
        if (pthread_equal(t->th, thread)) {
            __atomic_store_n(&t->detached, 1, __ATOMIC_RELEASE);
            break;
        }
    }
    mutexUnlock(&g_tramp_mutex);
    reap_detached();
    return 0;
}

// libnx puts every thread on the process' default core (0). SDL's audio thread sharing a core with
// GLideN64 got about half of real time (25.6k of 48k frames/s, sometimes none: music cut out).
// ultramodern names its threads, so place them by role and leave core 0 to the main thread and
// SDL's audio thread:
//   core 2: "Gfx Thread" (GLideN64, presents with vsync)
//   core 1: game threads (only one runs at a time), VI, timer and RSP task threads
#define GFX_CORE 2
#define GAME_CORE 1

static int core_for(const char* name) {
    if (strcmp(name, "Gfx Thread") == 0) {
        return GFX_CORE;
    }
    static const char* const game_side[] = {"VI Thread", "Timer Thread", "SP Task Thread", "Game", "PokemonStadium"};
    for (size_t i = 0; i < sizeof(game_side) / sizeof(game_side[0]); i++) {
        if (strncmp(name, game_side[i], strlen(game_side[i])) == 0) {
            return GAME_CORE;
        }
    }
    return -1;
}

// Threads that told us their name, for the stall sampler in main_switch.cpp (svcSetThreadActivity + svcGetThreadContext3
// work on the threads of our own process).
// The RSP task thread (emulated audio / JPEG microcode, tens of ms per task) shares core 1 with the guest threads. Horizon only
// time-slices threads of one priority class, never across priorities, so a guest thread busy-waiting for an RSP task (Stadium's
// JPEG decodes: `while (func_80001C90() == 0) {}`) at 0x30 starved this thread at the default 0x3B for 10-30 s per decode.
// One step above the guest threads (0x30) makes it preempt the waiter the moment a task arrives.
#define PSRNX_SP_THREAD_NAME "SP Task Thread"
#define PSRNX_SP_THREAD_PRIORITY 0x2F

#define PSRNX_MAX_THREADS 32
static struct { Handle handle; char name[24]; } g_threads[PSRNX_MAX_THREADS];
static volatile int g_thread_count;

void psrnx_place_thread(const char* name) {
    int core = core_for(name);
    if (core >= 0) {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    }
    const Thread* self = threadGetSelf();
    if (self != NULL) {
        for (int i = 0; i < g_thread_count && i < PSRNX_MAX_THREADS; i++) {
            if (g_threads[i].handle == self->handle) return;
        }
        int slot = __atomic_fetch_add(&g_thread_count, 1, __ATOMIC_SEQ_CST);
        if (slot < PSRNX_MAX_THREADS) {
            strncpy(g_threads[slot].name, name, sizeof(g_threads[slot].name) - 1);
            g_threads[slot].handle = self->handle;
        }
    }
    if (strcmp(name, PSRNX_SP_THREAD_NAME) == 0) {
        const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, PSRNX_SP_THREAD_PRIORITY);
        printf("[PSRNX] SP Task Thread priority 0x%x: rc=0x%x\n", PSRNX_SP_THREAD_PRIORITY, rc);
    }
}

int psrnx_thread_count(void) {
    int n = g_thread_count;
    return n > PSRNX_MAX_THREADS ? PSRNX_MAX_THREADS : n;
}

const char* psrnx_thread_name(int i) {
    return g_threads[i].name;
}

// Where a registered thread is executing right now. The thread is paused only for the two SVCs (no allocation, no
// stdio, nothing that could need a lock the paused thread holds).
bool psrnx_sample_thread(int i, uint64_t* pc, uint64_t* lr) {
    const Handle handle = g_threads[i].handle;
    ThreadContext ctx;
    if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused))) return false;
    const Result rc = svcGetThreadContext3(&ctx, handle);
    svcSetThreadActivity(handle, ThreadActivity_Runnable);
    if (R_FAILED(rc)) return false;
    *pc = ctx.pc.x;
    *lr = ctx.lr;
    return true;
}


// ---- extended sampling: registers, a frame-pointer backtrace, and the owner of a mutex a thread is blocked on ----------
static int readable(uint64_t addr, size_t len) {
    MemoryInfo mi;
    u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) return 0;
    return (mi.perm & Perm_R) && addr >= mi.addr && addr + len <= mi.addr + mi.size;
}

static int walk_frames(uint64_t fp, uint64_t* out, int max) {
    int n = 0;
    uint64_t prev = 0;
    while (n < max && (fp & 7) == 0 && fp > prev && readable(fp, 16)) {
        const uint64_t* rec = (const uint64_t*)fp;
        out[n++] = rec[1];
        prev = fp;
        fp = rec[0];
    }
    return n;
}

// Pauses `handle` just long enough for two SVCs and a stack walk that only issues svcQueryMemory (no allocation, no
// locks that the paused thread could hold).
static int sample_handle(Handle handle, uint64_t* pc, uint64_t* lr, uint64_t* x0, uint64_t* x1, uint64_t* bt, int* nbt) {
    ThreadContext ctx;
    if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused))) return 0;
    const Result rc = svcGetThreadContext3(&ctx, handle);
    if (R_SUCCEEDED(rc)) {
        *pc = ctx.pc.x;
        *lr = ctx.lr;
        *x0 = ctx.cpu_gprs[0].x;
        *x1 = ctx.cpu_gprs[1].x;
        *nbt = walk_frames(ctx.fp, bt, PSRNX_BT_MAX);
    }
    svcSetThreadActivity(handle, ThreadActivity_Runnable);
    return R_SUCCEEDED(rc);
}

int psrnx_sample_thread_ex(int i, PsrnxSample* s) {
    memset(s, 0, sizeof(*s));
    if (!sample_handle(g_threads[i].handle, &s->pc, &s->lr, &s->x0, &s->x1, s->bt, &s->nbt)) return 0;
    const uint64_t pc = s->pc;
    s->idle = pc == (uint64_t)(uintptr_t)&svcWaitSynchronization || pc == (uint64_t)(uintptr_t)&svcSleepThread;
    if (pc == (uint64_t)(uintptr_t)&svcArbitrateLock) {
        // svcArbitrateLock(wait_tag = owner's handle (| 0x40000000 when others wait too), tag_location = the mutex)
        s->blocked_on_mutex = 1;
        s->mutex_addr = s->x1;
        s->owner_handle = (uint32_t)s->x0 & ~0x40000000u;
        strcpy(s->owner_name, "unregistered");
        for (int k = 0; k < g_thread_count && k < PSRNX_MAX_THREADS; k++) {
            if (g_threads[k].handle == s->owner_handle) { strncpy(s->owner_name, g_threads[k].name, sizeof(s->owner_name) - 1); break; }
        }
        uint64_t x0, x1;
        if (s->owner_handle != g_threads[i].handle) {
            sample_handle(s->owner_handle, &s->owner_pc, &s->owner_lr, &x0, &x1, s->owner_bt, &s->owner_nbt);
        }
    }
    return 1;
}

// Runtime address of a function whose link address `nm` knows: converts the sampled PCs back to ELF addresses.
uintptr_t psrnx_code_marker(void) {
    return (uintptr_t)&psrnx_place_thread;
}

// ultramodern::stop_thread: waits for a std::thread to end before joining it. Its native handle is libnx's
// struct __pthread_t, which starts with the libnx Thread.
bool psrnx_wait_thread_exit(void* native_handle, u64 timeout_ns) {
    const Thread* thread = (const Thread*)native_handle;
    return R_SUCCEEDED(waitSingleHandle(thread->handle, timeout_ns));
}

// ultramodern::ThreadPriority order (Low, Normal, High, VeryHigh, Critical) to HOS priorities,
// where lower is more urgent; 0x2C is libnx's main thread, 0x3B its default for new threads.
void psrnx_set_thread_priority(int level) {
    static const u32 hos_priority[] = {0x3C, 0x3B, 0x30, 0x2D, 0x2C};
    const Thread* self = threadGetSelf();
    if (self != NULL) {
        for (int i = 0; i < g_thread_count && i < PSRNX_MAX_THREADS; i++) {
            if (g_threads[i].handle == self->handle && strcmp(g_threads[i].name, PSRNX_SP_THREAD_NAME) == 0) {
                svcSetThreadPriority(CUR_THREAD_HANDLE, PSRNX_SP_THREAD_PRIORITY);
                return;
            }
        }
    }
    if (level >= 0 && level < (int)(sizeof(hos_priority) / sizeof(hos_priority[0]))) {
        svcSetThreadPriority(CUR_THREAD_HANDLE, hos_priority[level]);
    }
}

// Loop back-edge hook. Generated code calls ultramodern_scheduler_tick_vram(pc) on EVERY back-edge of every loop; the
// runtime's version stamps a std::chrono clock (gettimeofday: an IPC to the time service on Horizon) plus five atomics
// before it does the actual yield check, which made loop-heavy loading code crawl. Linked with
// -Wl,--wrap=ultramodern_scheduler_tick_vram: keep the yield check (ultramodern_scheduler_tick) and only remember the
// pc, which the stall sampler reports. Plain stores on purpose: only one guest thread runs at a time, and this is
// diagnostics.
void ultramodern_scheduler_tick(void);
volatile uint32_t psrnx_loop_pc;
volatile uint64_t psrnx_loop_ticks;

void __wrap_ultramodern_scheduler_tick_vram(uint32_t pc) {
    psrnx_loop_pc = pc;
    psrnx_loop_ticks = psrnx_loop_ticks + 1;
    ultramodern_scheduler_tick();
}
