// SDL GameController -> N64 pads for up to four players (Stadium is a four-player game).
// Layout follows the NSO N64 convention used by DK64Recomp-NX: A->A, B->B, ZL->Z, L->L, R/ZR->R, +->Start,
// right stick->C buttons, D-pad->D-pad, left stick->analog stick and (left_stick_dpad) D-pad too. Controllers are assigned to N64 ports in the order they appear.
// Ports 1-3 answer as present when a pad is assigned or a Transfer Pak cart is configured for them.
// Joy-Con pairing and handheld behaviour of SDL's Switch backend is only verified for one player.
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <SDL.h>

#include "../src/main/transfer_pak.h"
#include "psrnx.h"
#include "stick_dpad.h"
#include "ultramodern/input.hpp"

namespace {
    constexpr int PORTS = 4;

    constexpr uint16_t N64_A = 0x8000;
    constexpr uint16_t N64_B = 0x4000;
    constexpr uint16_t N64_Z = 0x2000;
    constexpr uint16_t N64_START = 0x1000;
    constexpr uint16_t N64_DPAD_UP = 0x0800;
    constexpr uint16_t N64_DPAD_DOWN = 0x0400;
    constexpr uint16_t N64_DPAD_LEFT = 0x0200;
    constexpr uint16_t N64_DPAD_RIGHT = 0x0100;
    constexpr uint16_t N64_L = 0x0020;
    constexpr uint16_t N64_R = 0x0010;
    constexpr uint16_t N64_C_UP = 0x0008;
    constexpr uint16_t N64_C_DOWN = 0x0004;
    constexpr uint16_t N64_C_LEFT = 0x0002;
    constexpr uint16_t N64_C_RIGHT = 0x0001;

    // Same left-stick dead zone (raw SDL units) as the PC runner's get_n64_input.
    constexpr int32_t LSTICK_DEADZONE = 8000;
    constexpr int32_t C_BUTTON_THRESHOLD = 16000;
    constexpr uint16_t RUMBLE_STRENGTH = 0x6000;
    constexpr uint32_t RUMBLE_PULSE_MS = 100;

    // upstream main.cpp calls apply_stick_deadzone(), which exists in neither mstan/N64ModernRuntime
    // (any revision) nor N64Recomp/N64ModernRuntime: it was in the author's unpublished runtime tree. This is a
    // re-derivation from main.cpp's comment (normalised [-1, 1], dead zone removed and rescaled), not a copy.
    float apply_stick_deadzone(int32_t raw, int32_t deadzone) {
        const int32_t magnitude = raw < 0 ? -raw : raw;
        if (magnitude <= deadzone) return 0.0f;
        float scaled = static_cast<float>(magnitude - deadzone) / static_cast<float>(32767 - deadzone);
        if (scaled > 1.0f) scaled = 1.0f;
        return raw < 0 ? -scaled : scaled;
    }

    // Only the game thread calls this, so the per-port state needs no lock.
    std::array<psrnx::StickDpad, PORTS> g_stick_dpad{};

    std::array<std::atomic<SDL_GameController*>, PORTS> g_pads{};
    std::array<std::atomic_bool, PORTS> g_rumble{};

    int free_port() {
        for (int port = 0; port < PORTS; port++) {
            if (g_pads[port].load() == nullptr) return port;
        }
        return -1;
    }

    bool already_open(SDL_JoystickID id) {
        for (auto& slot : g_pads) {
            SDL_GameController* pad = slot.load();
            if (pad != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id) return true;
        }
        return false;
    }
}

// Called on the main thread, which owns the SDL event queue (main_switch.cpp update_gfx).
void psrnx_input_handle_event(const SDL_Event& event) {
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
        const int port = free_port();
        if (port >= 0) {
            SDL_GameController* pad = SDL_GameControllerOpen(event.cdevice.which);
            if (pad != nullptr && !already_open(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)))) {
                g_pads[port].store(pad);
            }
        }
    } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        for (auto& slot : g_pads) {
            SDL_GameController* pad = slot.load();
            if (pad != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == event.cdevice.which) {
                // ponytail: the handle is dropped, not closed, because the game thread may be reading it.
                slot.store(nullptr);
            }
        }
    }
}

// Game thread. The main thread pumps SDL events every update_gfx, so controller state is already fresh.
bool psrnx_get_input(int controller_num, uint16_t* buttons_out, float* x_out, float* y_out) {
    *buttons_out = 0;
    *x_out = 0.0f;
    *y_out = 0.0f;
    if (controller_num < 0 || controller_num >= PORTS) return false;
    SDL_GameController* pad = g_pads[controller_num].load();
    if (pad == nullptr) {
        // No pad on this port: it still answers (idle) if a Transfer Pak cart is configured for it.
        return pkmnstadium::transfer_pak::has_transfer_pak(controller_num);
    }

    struct Binding { SDL_GameControllerButton button; uint16_t n64; };
    // SDL names the face buttons by position (A below, B right); the labels printed on a Switch pad are the other way
    // round. With nintendo_buttons the game's A (confirm) is the pad's A (right) and its B (cancel) the pad's B (below).
    const bool nintendo = psrnx::config().nintendo_buttons;
    const Binding bindings[] = {
        {nintendo ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A, N64_A},
        {nintendo ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B, N64_B},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, N64_L},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, N64_R},
        {SDL_CONTROLLER_BUTTON_START, N64_START},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, N64_DPAD_UP},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, N64_DPAD_DOWN},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, N64_DPAD_LEFT},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, N64_DPAD_RIGHT},
    };
    uint16_t buttons = 0;
    for (const Binding& binding : bindings) {
        if (SDL_GameControllerGetButton(pad, binding.button)) buttons |= binding.n64;
    }
    // ZL/ZR are analog axes in SDL.
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) buttons |= N64_Z;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) buttons |= N64_R;

    const int32_t rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
    const int32_t ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
    if (rx < -C_BUTTON_THRESHOLD) buttons |= N64_C_LEFT;
    if (rx > C_BUTTON_THRESHOLD) buttons |= N64_C_RIGHT;
    if (ry < -C_BUTTON_THRESHOLD) buttons |= N64_C_UP;    // SDL's Y axes point down
    if (ry > C_BUTTON_THRESHOLD) buttons |= N64_C_DOWN;

    const int32_t lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    const int32_t ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (psrnx::config().left_stick_dpad) buttons |= g_stick_dpad[controller_num].update(lx, ly);
    // As on PC: normalised [-1, 1] after the dead zone, Y inverted for the N64.
    *buttons_out = buttons;
    *x_out = apply_stick_deadzone(lx, LSTICK_DEADZONE);
    *y_out = -apply_stick_deadzone(ly, LSTICK_DEADZONE);
    return true;
}

void psrnx_poll_input() {}

void psrnx_set_rumble(int controller_num, bool on) {
    if (controller_num >= 0 && controller_num < PORTS) g_rumble[controller_num].store(on);
}

// Per VI (events callbacks_t::vi_callback): re-arm the short pulse while the game keeps rumble on.
void psrnx_update_rumble() {
    for (int port = 0; port < PORTS; port++) {
        SDL_GameController* pad = g_pads[port].load();
        if (pad != nullptr && g_rumble[port].load()) {
            SDL_GameControllerRumble(pad, RUMBLE_STRENGTH, RUMBLE_STRENGTH, RUMBLE_PULSE_MS);
        }
    }
}

ultramodern::input::connected_device_info_t psrnx_get_connected_device_info(int controller_num) {
    ultramodern::input::connected_device_info_t info{};
    const bool has_pad = controller_num >= 0 && controller_num < PORTS && g_pads[controller_num].load() != nullptr;
    const bool has_transfer_pak = pkmnstadium::transfer_pak::has_transfer_pak(controller_num);
    // Port 0 always reports present (libultra's osContInit asks before any pad has been opened).
    info.connected_device = (controller_num == 0 || has_pad || has_transfer_pak)
                                ? ultramodern::input::Device::Controller
                                : ultramodern::input::Device::None;
    // ultramodern has no TransferPak value: RumblePak is the presence stand-in (see main.cpp).
    info.connected_pak = has_transfer_pak ? ultramodern::input::Pak::RumblePak : ultramodern::input::Pak::None;
    return info;
}
