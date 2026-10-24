// Switch audio for Stadium: the clock-domain bridge path of src/main/main.cpp (the default there), without the
// Windows-only parts (WASAPI render path, device selection, capture/debug rings, legacy SDL queue path).
//
// Why this and not DK64-NX's queue path: Stadium's pacing is closed-loop. The game sizes every audio task from
// osAiGetLength, which get_frames_remaining() answers from the bridge fill minus PSR_AUDIO_LEAD_MS; the legacy path
// (SDL_AudioCVT per chunk, decimation valve) is what crackled on PC and is not carried over.
//
// The logic below is main.cpp lines ~641-780 and ~1075-1130 reduced to the bridge path.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

#include <SDL.h>

#define RECOMP_AUDIO_DRC_IMPL
#include "../src/main/recomp_audio_drc.h"

#include "psrnx.h"

namespace {
    constexpr int input_channels = 2;
    constexpr int output_channels = 2;

    rab_bridge g_bridge;
    bool g_bridge_ready = false;
    SDL_mutex* g_audio_mtx = nullptr;
    SDL_AudioDeviceID g_audio_device = 0;
    uint32_t g_sample_rate = 32000;      // the game's rate (osAiSetFrequency)
    uint32_t g_output_rate = 48000;
    uint32_t g_bridge_src = 0;
    std::atomic<double> g_last_pull_ms{0.0};

    double env_double(const char* name, double fallback) {
        const char* value = std::getenv(name);
        return (value == nullptr || *value == '\0') ? fallback : std::atof(value);
    }

    double now_ms() {
        return static_cast<double>(SDL_GetPerformanceCounter()) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
    }

    // Caller holds g_audio_mtx.
    void bridge_reinit_locked() {
        if (g_bridge_ready) {
            rab_free(&g_bridge);
            g_bridge_ready = false;
        }
        rab_config config;
        rab_config_defaults(&config);
        config.channels = output_channels;
        config.source_rate = static_cast<double>(g_sample_rate);
        config.host_rate = static_cast<double>(g_output_rate);
        config.target_ms = env_double("PSR_AUDIO_TARGET_MS", 24.0);
        config.ring_ms = 300.0;
        // No rate warping: the game's own osAiGetLength loop is the rate controller (see main.cpp for the
        // measurements that led to this).
        config.max_correction = env_double("PSR_AUDIO_MAX_CORR", 0.0);
        g_bridge_ready = rab_init(&g_bridge, &config) == 0;
        g_bridge_src = g_sample_rate;
    }

    // SDL audio thread.
    void audio_callback(void*, Uint8* stream, int len) {
        const int frames = len / static_cast<int>(output_channels * sizeof(float));
        if (!g_bridge_ready) {
            SDL_memset(stream, 0, static_cast<size_t>(len));
            return;
        }
        static std::vector<int16_t> pulled;
        if (static_cast<int>(pulled.size()) < frames * output_channels) {
            pulled.resize(static_cast<size_t>(frames) * output_channels);
        }
        SDL_LockMutex(g_audio_mtx);
        rab_pull(&g_bridge, pulled.data(), frames);
        SDL_UnlockMutex(g_audio_mtx);
        g_last_pull_ms.store(now_ms(), std::memory_order_relaxed);
        float* out = reinterpret_cast<float*>(stream);
        for (int i = 0; i < frames * output_channels; ++i) {
            out[i] = static_cast<float>(pulled[static_cast<size_t>(i)]) * (1.0f / 32768.0f);
        }
    }
}

// ultramodern::audio_callbacks_t
void psrnx_queue_samples(int16_t* audio_data, size_t sample_count) {
    const size_t frames = sample_count / input_channels;
    psrnx_diag::audio_pushes.fetch_add(1, std::memory_order_relaxed);
    psrnx_diag::audio_frames.fetch_add(frames, std::memory_order_relaxed);
    if (frames == 0 || !g_bridge_ready) return;
    static std::vector<int16_t> push;
    if (push.size() < frames * output_channels) push.resize(frames * output_channels);
    // libultra interleaves R,L per word; emit L,R. 0.5 headroom as on PC.
    for (size_t f = 0; f < frames; ++f) {
        int l = static_cast<int>(lrintf(static_cast<float>(audio_data[f * input_channels + 1]) * 0.5f));
        int r = static_cast<int>(lrintf(static_cast<float>(audio_data[f * input_channels + 0]) * 0.5f));
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        push[f * output_channels + 0] = static_cast<int16_t>(l);
        push[f * output_channels + 1] = static_cast<int16_t>(r);
    }
    SDL_LockMutex(g_audio_mtx);
    if (g_bridge_src != g_sample_rate) bridge_reinit_locked();
    rab_push(&g_bridge, push.data(), static_cast<int>(frames));
    SDL_UnlockMutex(g_audio_mtx);
}

size_t psrnx_get_frames_remaining() {
    if (!g_bridge_ready) return 0;
    SDL_LockMutex(g_audio_mtx);
    const double fill_ms = rab_fill_ms(&g_bridge);
    SDL_UnlockMutex(g_audio_mtx);
    // The lead sets the steady-state ring depth: the game drives the REPORTED remaining to its own small target, so
    // the real fill is that target plus the lead. 50 ms keeps the ring from running dry between callback pulls.
    static const double lead_ms = env_double("PSR_AUDIO_LEAD_MS", 50.0);
    double frames = fill_ms * 0.001 * static_cast<double>(g_sample_rate);
    // The hardware AI_LEN register drains continuously; the ring's read index moves in whole callback chunks.
    // Interpolate the drain since the last pull (PSR_AI_SMOOTH=0 restores the stepped readback).
    static const bool smooth = [] {
        const char* value = std::getenv("PSR_AI_SMOOTH");
        return !(value != nullptr && value[0] == '0' && value[1] == '\0');
    }();
    if (smooth) {
        const double anchor = g_last_pull_ms.load(std::memory_order_relaxed);
        if (anchor > 0.0) {
            double elapsed_ms = now_ms() - anchor;
            if (elapsed_ms < 0.0) elapsed_ms = 0.0;
            if (elapsed_ms > 50.0) elapsed_ms = 50.0;
            frames -= elapsed_ms * 0.001 * static_cast<double>(g_sample_rate);
            if (frames < 0.0) frames = 0.0;
        }
    }
    const double lag = lead_ms * 0.001 * static_cast<double>(g_sample_rate);
    return static_cast<size_t>(frames > lag ? frames - lag : 0.0);
}

void psrnx_set_frequency(uint32_t freq) {
    std::printf("[PSRNX] audio: game frequency %u Hz\n", freq);
    g_sample_rate = freq;
    if (g_audio_device != 0) {
        SDL_LockMutex(g_audio_mtx);
        bridge_reinit_locked();
        SDL_UnlockMutex(g_audio_mtx);
    }
}

bool psrnx_reset_audio(uint32_t output_freq) {
    if (g_audio_mtx == nullptr) g_audio_mtx = SDL_CreateMutex();
    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(output_freq);
    desired.format = AUDIO_F32;
    desired.channels = output_channels;
    desired.samples = static_cast<Uint16>(psrnx::config().audio_buffer_frames);
    desired.callback = audio_callback;
    SDL_AudioSpec obtained{};
    g_audio_device = SDL_OpenAudioDevice(nullptr, false, &desired, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (g_audio_device == 0) {
        std::fprintf(stderr, "[PSRNX] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }
    // One resampler only: the bridge targets the rate the device really opened at.
    g_output_rate = obtained.freq > 0 ? static_cast<uint32_t>(obtained.freq) : output_freq;
    SDL_LockMutex(g_audio_mtx);
    bridge_reinit_locked();
    SDL_UnlockMutex(g_audio_mtx);
    std::printf("[PSRNX] audio: driver=%s obtained=%dHz %dch %d frames, bridge %u -> %u Hz ready=%d\n",
                SDL_GetCurrentAudioDriver() != nullptr ? SDL_GetCurrentAudioDriver() : "?", obtained.freq,
                obtained.channels, obtained.samples, g_sample_rate, g_output_rate, g_bridge_ready ? 1 : 0);
    SDL_PauseAudioDevice(g_audio_device, 0);
    return true;
}

void psrnx_close_audio() {
    if (g_audio_device != 0) {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }
}
