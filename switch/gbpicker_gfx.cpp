#include "gbpicker_gfx.h"

#include <algorithm>
#include <cmath>

namespace gbpicker {
using skin::rgba;

namespace {
    inline uint8_t ch(uint32_t px, int shift) { return static_cast<uint8_t>(px >> shift); }

    // out = src over dst with straight alpha (alpha 0..255 additionally scaled by `extra` 0..255).
    inline uint32_t over(uint32_t dst, uint32_t src, int extra = 255) {
        const int a = ch(src, 24) * extra / 255;
        if (a <= 0) return dst;
        if (a >= 255) return src | 0xFF000000u;
        const int r = (ch(src, 0) * a + ch(dst, 0) * (255 - a)) / 255;
        const int g = (ch(src, 8) * a + ch(dst, 8) * (255 - a)) / 255;
        const int b = (ch(src, 16) * a + ch(dst, 16) * (255 - a)) / 255;
        return rgba(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
    }

    uint32_t lerp_px(uint32_t a, uint32_t b, float t) {
        auto mix = [&](int shift) { return static_cast<uint8_t>(ch(a, shift) + (ch(b, shift) - ch(a, shift)) * t + 0.5f); };
        return rgba(mix(0), mix(8), mix(16), mix(24));
    }

    // Bilinear sample with clamped edges; (x, y) in texel space, texel centres at +0.5.
    uint32_t sample(const skin::Image& im, float x, float y) {
        x -= 0.5f; y -= 0.5f;
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        auto at = [&](int xx, int yy) { return im.px[static_cast<size_t>(std::clamp(yy, 0, im.h - 1)) * im.w + std::clamp(xx, 0, im.w - 1)]; };
        return lerp_px(lerp_px(at(x0, y0), at(x0 + 1, y0), fx), lerp_px(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
    }

    // Stadium palette.
    constexpr uint32_t kBlue = rgba(30, 30, 130), kRed = rgba(100, 30, 30), kGreen = rgba(0, 96, 40), kWhite = rgba(255, 255, 255);
    constexpr int kPanelX0 = 55, kPanelStep = 135, kPanelW = 125, kPanelY = 137, kPanelH = 198;
}

Gfx::Gfx(const skin::Skin& skin) : skin_(skin), canvas_(static_cast<size_t>(kW) * kH, rgba(0, 0, 0)) {}

void Gfx::fill(int x, int y, int w, int h, uint32_t color) {
    for (int yy = std::max(0, y); yy < std::min(kH, y + h); ++yy)
        for (int xx = std::max(0, x); xx < std::min(kW, x + w); ++xx) {
            uint32_t& d = canvas_[static_cast<size_t>(yy) * kW + xx];
            d = over(d, color);
        }
}

// Rounded translucent panel as the game builds it (fragment33 func_81301668): four 16x16 corner masks tinted with the
// colour, solid rectangles between them. The colour's alpha is the panel's opacity.
void Gfx::panel(int x, int y, int w, int h, uint32_t color) {
    const int c = 16;
    const uint32_t solid = color;
    auto corner = [&](const skin::Image& mask, int cx, int cy) {
        for (int j = 0; j < c; ++j)
            for (int i = 0; i < c; ++i) {
                const int a = ch(mask.px[static_cast<size_t>(j) * c + i], 24);
                const int xx = cx + i, yy = cy + j;
                if (xx < 0 || yy < 0 || xx >= kW || yy >= kH) continue;
                uint32_t& d = canvas_[static_cast<size_t>(yy) * kW + xx];
                d = over(d, (solid & 0x00FFFFFFu) | 0xFF000000u, a * ch(solid, 24) / 255);
            }
    };
    if (skin_.corner[0].empty()) { fill(x, y, w, h, color); return; }
    corner(skin_.corner[0], x, y);
    corner(skin_.corner[1], x + w - c, y);
    corner(skin_.corner[2], x, y + h - c);
    corner(skin_.corner[3], x + w - c, y + h - c);
    fill(x + c, y, w - 2 * c, c, color);
    fill(x, y + c, w, h - 2 * c, color);
    fill(x + c, y + h - c, w - 2 * c, c, color);
}

void Gfx::blit(const skin::Image& im, float x, float y, float scale, uint8_t alpha) {
    if (im.empty()) return;
    const int w = static_cast<int>(im.w * scale + 0.5f), h = static_cast<int>(im.h * scale + 0.5f);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const int xx = static_cast<int>(x) + i, yy = static_cast<int>(y) + j;
            if (xx < 0 || yy < 0 || xx >= kW || yy >= kH) continue;
            const uint32_t s = scale == 1.0f ? im.px[static_cast<size_t>(j) * im.w + i] : sample(im, (i + 0.5f) / scale, (j + 0.5f) / scale);
            uint32_t& d = canvas_[static_cast<size_t>(yy) * kW + xx];
            d = over(d, s, alpha);
        }
}

int Gfx::text_width(const std::string& s, float scale) const {
    if (!skin_.font.ok()) return static_cast<int>(s.size()) * static_cast<int>(8 * scale);
    int w = 0;
    for (char c : s) w += skin_.font.widths[skin::font_tile_for(c)];
    return static_cast<int>(w * scale + 0.5f);
}

// Font tiles are IA8: intensity (high nibble) times the tint colour, coverage (low nibble). Drawn with bilinear sampling
// when scaled.
void Gfx::text(const std::string& s, int x, int y, float scale, uint32_t tint) {
    if (!skin_.font.ok()) return;
    const auto& f = skin_.font;
    float pen = static_cast<float>(x);
    for (char c : s) {
        const int tile = skin::font_tile_for(c);
        const uint8_t* g = &f.tiles[static_cast<size_t>(tile) * skin::Font::kCellW * skin::Font::kCellH];
        const int gw = static_cast<int>(skin::Font::kCellW * scale + 0.5f), gh = static_cast<int>(skin::Font::kCellH * scale + 0.5f);
        if (tile != 0) {
            for (int j = 0; j < gh; ++j)
                for (int i = 0; i < gw; ++i) {
                    const int xx = static_cast<int>(pen) + i, yy = y + j;
                    if (xx < 0 || yy < 0 || xx >= kW || yy >= kH) continue;
                    float inten, cov;
                    if (scale == 1.0f) {
                        const uint8_t v = g[j * skin::Font::kCellW + i];
                        inten = (v >> 4) / 15.0f; cov = (v & 15) / 15.0f;
                    } else {
                        const float sx = (i + 0.5f) / scale - 0.5f, sy = (j + 0.5f) / scale - 0.5f;
                        const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
                        const float fx = sx - x0, fy = sy - y0;
                        auto at = [&](int xx2, int yy2, bool alpha) {
                            if (xx2 < 0 || yy2 < 0 || xx2 >= skin::Font::kCellW || yy2 >= skin::Font::kCellH) return alpha ? 0.0f : 0.0f;
                            const uint8_t v = g[yy2 * skin::Font::kCellW + xx2];
                            return (alpha ? (v & 15) : (v >> 4)) / 15.0f;
                        };
                        // Intensity is only meaningful where there is coverage: weight it by coverage so the dark rim
                        // does not bleed into empty texels.
                        float ci = 0, cw = 0;
                        const float wts[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                        const int ox[4] = {0, 1, 0, 1}, oy[4] = {0, 0, 1, 1};
                        cov = 0;
                        for (int k = 0; k < 4; ++k) {
                            const float a = at(x0 + ox[k], y0 + oy[k], true);
                            cov += wts[k] * a;
                            ci += wts[k] * a * at(x0 + ox[k], y0 + oy[k], false);
                            cw += wts[k] * a;
                        }
                        inten = cw > 0 ? ci / cw : 0;
                    }
                    if (cov <= 0.0f) continue;
                    const uint8_t r = static_cast<uint8_t>(inten * ch(tint, 0)), gg = static_cast<uint8_t>(inten * ch(tint, 8)), b = static_cast<uint8_t>(inten * ch(tint, 16));
                    uint32_t& d = canvas_[static_cast<size_t>(yy) * kW + xx];
                    d = over(d, rgba(r, gg, b, static_cast<uint8_t>(cov * 255)));
                }
        }
        pen += f.widths[tile] * scale;
    }
}

std::string Gfx::fit(const std::string& s, int max_width, float scale) const {
    if (text_width(s, scale) <= max_width) return s;
    std::string t = s;
    while (!t.empty() && text_width(t + "...", scale) > max_width) t.pop_back();
    return t + "...";
}

void Gfx::text_centered(const std::string& s, int cx, int y, int max_width, float scale, uint32_t tint) {
    const std::string t = fit(s, max_width, scale);
    text(t, cx - text_width(t, scale) / 2, y, scale, tint);
}

// The menu background (fragment57 func_82D00088): the 128x64 I4 logo tile repeats at 1:1 over the screen; texel intensity 0 is
// the primitive colour (120,100,40), intensity 1 the environment colour (180,180,100).
void Gfx::background() {
    const skin::Image& t = skin_.logo_tile;
    const int pr = 120, pg = 100, pb = 40, er = 180, eg = 180, eb = 100;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const int i = t.empty() ? 128 : ch(t.px[static_cast<size_t>(y % t.h) * t.w + (x % t.w)], 0);
            canvas_[static_cast<size_t>(y) * kW + x] = rgba(static_cast<uint8_t>(pr + (er - pr) * i / 255), static_cast<uint8_t>(pg + (eg - pg) * i / 255),
                                                            static_cast<uint8_t>(pb + (eb - pb) * i / 255));
        }
}

void Gfx::heading(const char* line1, const char* line2) {
    panel((kW - 176) / 2, 40, 176, 57, rgba(30, 30, 130, 170));   // centred horizontally
    text_centered(line1, kW / 2, 42, 160, 0.85f);
    text_centered(line2, kW / 2, 64, 160, 0.85f);
}

void Gfx::ok_button(const char* label) {
    panel(463, 55, 122, 42, rgba(255, 255, 255, 255));
    panel(466, 58, 116, 36, kGreen);
    text_centered(label, 524, 61, 110);
}

void Gfx::footer(const View& v, const std::string& line1, const std::string& line2) {
    panel(52, 348, 533, 94, kRed);
    // hint line: "A Cartridge  X Save ..." drawn with the key letters in yellow
    int x = 76;
    const float s = 0.7f;
    text(fit(line1, 490, 0.8f), x, 358);
    text(fit(line2, 490, 0.8f), x, 382);
    std::string hint;
    int hx = x;
    const int hy = (line1.empty() && line2.empty()) ? 385 : 410;
    for (size_t i = 0; i < v.hints.size(); ++i) {
        const std::string& key = v.hints[i].first;
        text(key, hx, hy, s, rgba(255, 220, 90));
        hx += text_width(key, s) + 3;
        text(v.hints[i].second, hx, hy, s);
        hx += text_width(v.hints[i].second, s) + 11;
    }
}

void Gfx::main_screen(const View& v) {
    heading("Game Pak", "Setup");
    ok_button("+ Start");
    for (int i = 0; i < 4; ++i) {
        const PortView& p = v.ports[i];
        const int x = kPanelX0 + kPanelStep * i;
        const int y = kPanelY;
        if (i == v.port) panel(x - 5, y - 5, kPanelW + 10, kPanelH + 10, rgba(235, 190, 40, 220));
        uint32_t color = kBlue;
        if (p.used && (p.problem || p.name == "No save")) color = kRed;
        panel(x, y, kPanelW, kPanelH, color);
        text(std::to_string(i + 1), x + 9, y + 6, 0.8f);
        if (!p.used) {
            text_centered("Game Pak", x + kPanelW / 2, y + 60, kPanelW - 10);
            text_centered("None", x + kPanelW / 2, y + 82, kPanelW - 10);
            continue;
        }
        const skin::Image* cart = nullptr;
        switch (p.kind) {
        case Kind::Red: cart = &skin_.cart[skin::kCartRed]; break;
        case Kind::Green: cart = &skin_.cart[skin::kCartGreen]; break;
        case Kind::Blue: cart = &skin_.cart[skin::kCartBlue]; break;
        case Kind::Yellow: cart = &skin_.cart[skin::kCartYellow]; break;
        default: break;
        }
        if (cart != nullptr) blit(*cart, static_cast<float>(x + 40), static_cast<float>(y + 28), 0.65f);
        text_centered(p.game, x + kPanelW / 2, y + 76, kPanelW - 10);
        text_centered(p.name, x + kPanelW / 2, y + 100, kPanelW - 10);
        if (!p.id.empty()) text_centered(p.id, x + kPanelW / 2, y + 122, kPanelW - 10);
        if (p.name == "No save") text_centered("will start", x + kPanelW / 2, y + 126, kPanelW - 10, 0.65f), text_centered("without data", x + kPanelW / 2, y + 146, kPanelW - 10, 0.65f);
    }
    const PortView& sel = v.ports[v.port];
    std::string l1 = "Port " + std::to_string(v.port + 1) + ": " + (sel.used ? "ROM  " + sel.rom_file : "no cartridge");
    std::string l2 = sel.used ? (sel.save_file.empty() ? "No save file" : "Save  " + sel.save_file) : "";
    if (!v.notices.empty()) l2 = v.notices.front();
    footer(v, l1, l2);
}

void Gfx::list_screen(const View& v) {
    const bool rom = v.type == View::Type::PickRom;
    heading(rom ? "Choose" : "Choose", rom ? "Game Pak" : "Save File");
    panel(52, 112, 533, 226, kBlue);
    text(fit(v.subtitle, 500, 0.75f), 70, 112 + 8, 0.75f, rgba(255, 220, 90));
    const int rows = 6, row_h = 30, top = 112 + 32;
    int first = 0;
    const int n = static_cast<int>(v.items.size());
    if (v.cursor >= rows / 2) first = v.cursor - rows / 2 + 1;
    if (first + rows > n) first = std::max(0, n - rows);
    for (int r = 0; r < rows && first + r < n; ++r) {
        const int i = first + r;
        const int y = top + r * row_h;
        if (i == v.cursor) {
            panel(62, y - 2, 513, row_h - 2, kRed);
            fill(70, y + 7, 8, 10, kWhite);   // simple marker
        }
        text(fit(v.items[i], 190, 0.8f), 86, y, 0.8f);
        if (i < static_cast<int>(v.details.size())) text(fit(v.details[i], 290, 0.65f), 280, y + 3, 0.65f, rgba(210, 210, 255));
    }
    if (first > 0) text("...", 560, 112 + 4);
    if (first + rows < n) text("...", 560, 112 + 226 - 24);
    footer(v, "", "");
}

void Gfx::message_screen(const View& v) {
    heading("Game Pak", "Setup");
    panel(52, 112, 533, 150, kBlue);
    // word wrap
    std::string line, word;
    int y = 140;
    auto flush = [&]() { if (!line.empty()) { text(line, 76, y); y += 28; line.clear(); } };
    for (size_t i = 0; i <= v.message.size(); ++i) {
        const char c = i < v.message.size() ? v.message[i] : ' ';
        if (c == ' ') {
            if (!word.empty()) {
                const std::string t = line.empty() ? word : line + " " + word;
                if (text_width(t) > 480) { flush(); line = word; } else line = t;
                word.clear();
            }
        } else word += c;
    }
    flush();
    if (!v.hints.empty()) footer(v, "", "");
}

void Gfx::splash(const char* head1, const char* head2, const std::vector<std::string>& lines, float progress, const std::string& caption) {
    background();
    heading(head1, head2);
    panel(52, 130, 533, 210, kBlue);
    int y = 150;
    for (const std::string& l : lines) { text_centered(l, kW / 2, y, 490); y += 30; }
    if (progress >= 0.0f) {
        const int bx = 92, by = 270, bw = 453, bh = 26;
        panel(bx - 3, by - 3, bw + 6, bh + 6, rgba(255, 255, 255, 255));
        panel(bx, by, bw, bh, rgba(20, 20, 70, 255));
        const int fw = static_cast<int>(bw * std::clamp(progress, 0.0f, 1.0f));
        if (fw >= 32) panel(bx, by, fw, bh, kGreen);
        else if (fw > 0) fill(bx + 2, by + 2, fw, bh - 4, kGreen);
        if (!caption.empty()) text_centered(caption, kW / 2, by + bh + 8, 490, 0.7f, rgba(255, 220, 90));
    }
}

void Gfx::render(const View& v) {
    background();
    switch (v.type) {
    case View::Type::Main: main_screen(v); break;
    case View::Type::PickRom:
    case View::Type::PickSave: list_screen(v); break;
    case View::Type::Message: message_screen(v); break;
    }
}

void Gfx::present(uint32_t* dst, int dst_w, int dst_h, int stride_px, bool stretch) const {
    // Bilinear scale with 8-bit fixed-point weights; the two channel pairs (R,B) and (G,A) are mixed in one multiply each.
    // stretch: fill the whole target like the game's own menus do on the Switch (16:9); otherwise 4:3 centred with side bars.
    const float sy = stretch ? static_cast<float>(dst_h) / kH : std::min(static_cast<float>(dst_w) / kW, static_cast<float>(dst_h) / kH);
    const float sx = stretch ? static_cast<float>(dst_w) / kW : sy;
    const int out_w = static_cast<int>(kW * sx), out_h = static_cast<int>(kH * sy);
    const int ox = (dst_w - out_w) / 2, oy = (dst_h - out_h) / 2;
    struct Tap { int i0, i1, w; };
    auto taps = [&](int count, int src_size, float scale) {
        std::vector<Tap> t(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            const float s = (i + 0.5f) / scale - 0.5f;
            const int i0 = static_cast<int>(std::floor(s));
            t[static_cast<size_t>(i)] = {std::clamp(i0, 0, src_size - 1), std::clamp(i0 + 1, 0, src_size - 1), static_cast<int>((s - i0) * 256.0f)};
        }
        return t;
    };
    const std::vector<Tap> tx = taps(out_w, kW, sx), ty = taps(out_h, kH, sy);
    auto mix = [](uint32_t a, uint32_t b, int w) {
        const uint32_t rb = (((a & 0x00FF00FFu) * (256 - w) + (b & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
        const uint32_t ga = ((((a >> 8) & 0x00FF00FFu) * (256 - w) + ((b >> 8) & 0x00FF00FFu) * w)) & 0xFF00FF00u;
        return rb | ga;
    };
    for (int y = 0; y < dst_h; ++y) {
        uint32_t* row = dst + static_cast<size_t>(y) * stride_px;
        if (y < oy || y >= oy + out_h) { std::fill(row, row + dst_w, 0xFF000000u); continue; }
        const Tap& vy = ty[static_cast<size_t>(y - oy)];
        const uint32_t* r0 = &canvas_[static_cast<size_t>(vy.i0) * kW];
        const uint32_t* r1 = &canvas_[static_cast<size_t>(vy.i1) * kW];
        std::fill(row, row + ox, 0xFF000000u);
        for (int x = 0; x < out_w; ++x) {
            const Tap& hx = tx[static_cast<size_t>(x)];
            row[ox + x] = mix(mix(r0[hx.i0], r0[hx.i1], hx.w), mix(r1[hx.i0], r1[hx.i1], hx.w), vy.w) | 0xFF000000u;
        }
        std::fill(row + ox + out_w, row + dst_w, 0xFF000000u);
    }
}
}
