// Draws the cartridge picker in the style of Pokemon Stadium's "Game Pak Check" menu (rounded translucent panels, the game's
// font, coloured cartridge icons, the menu background), into a plain RGBA buffer. Portable C++17: the Switch front end
// copies the result into a libnx framebuffer, tests/gfx_preview.cpp writes it to an image.
//
// The menu is laid out on Stadium's own 640x480 canvas and scaled to the target: stretched to fill 16:9 like the game's own menus (widescreen=2), or centred 4:3 with side bars.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gbpicker.h"
#include "stadium_skin.h"

namespace gbpicker {
    class Gfx {
    public:
        static constexpr int kW = 640, kH = 480;

        explicit Gfx(const skin::Skin& skin);
        void render(const View& view);                                   // draws into the internal 640x480 canvas
        void present(uint32_t* dst, int dst_w, int dst_h, int stride_px, bool stretch = false) const;  // scales (bilinear) into dst, RGBA bytes: stretched to fill, or 4:3 centred
        const std::vector<uint32_t>& canvas() const { return canvas_; }

        // A waiting screen for the game's start-up (shader warm-up): the menu background, a heading, a panel with `lines` and,
        // when `progress` is 0..1, a bar below them with `caption` under it.
        void splash(const char* head1, const char* head2, const std::vector<std::string>& lines, float progress, const std::string& caption);

        // Text helpers (also used by the tests).
        int text_width(const std::string& s, float scale = 0.8f) const;

    private:
        void background();
        void fill(int x, int y, int w, int h, uint32_t color);
        void panel(int x, int y, int w, int h, uint32_t color);
        void blit(const skin::Image& im, float x, float y, float scale, uint8_t alpha = 255);
        void text(const std::string& s, int x, int y, float scale = 0.8f, uint32_t tint = 0xFFFFFFFFu);
        void text_centered(const std::string& s, int cx, int y, int max_width, float scale = 0.8f, uint32_t tint = 0xFFFFFFFFu);
        std::string fit(const std::string& s, int max_width, float scale) const;
        void heading(const char* line1, const char* line2);
        void ok_button(const char* label);
        void main_screen(const View& v);
        void list_screen(const View& v);
        void message_screen(const View& v);
        void footer(const View& v, const std::string& line1, const std::string& line2);

        const skin::Skin& skin_;
        std::vector<uint32_t> canvas_;
    };
}
