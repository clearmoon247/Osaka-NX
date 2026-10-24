// Left stick -> D-pad bits. Stadium's menus read only the D-pad (controller.c: buttonPressed & 0x800/0x400/0x200/0x100) and never
// the analog stick, so to navigate with the stick it also presses the D-pad. One direction at a time (the dominant axis), with a
// wider release threshold so a stick resting near the edge does not flicker. Raw SDL axis units (-32768..32767, Y down).
#pragma once

#include <cstdint>

namespace psrnx {
    constexpr int32_t kStickDpadOn = 18000;
    constexpr int32_t kStickDpadOff = 12000;
    constexpr uint16_t kDpadUp = 0x0800, kDpadDown = 0x0400, kDpadLeft = 0x0200, kDpadRight = 0x0100;

    struct StickDpad {
        bool up = false, down = false, left = false, right = false;

        uint16_t update(int32_t lx, int32_t ly) {
            const int32_t ax = lx < 0 ? -lx : lx, ay = ly < 0 ? -ly : ly;
            const bool horizontal_held = left || right, vertical_held = up || down;
            const bool horizontal = ax >= ay && ax >= (horizontal_held ? kStickDpadOff : kStickDpadOn);
            const bool vertical = ay > ax && ay >= (vertical_held ? kStickDpadOff : kStickDpadOn);
            left = horizontal && lx < 0;
            right = horizontal && lx > 0;
            up = vertical && ly < 0;      // SDL's Y axes point down
            down = vertical && ly > 0;
            return static_cast<uint16_t>((up ? kDpadUp : 0) | (down ? kDpadDown : 0) | (left ? kDpadLeft : 0) | (right ? kDpadRight : 0));
        }
    };
}
