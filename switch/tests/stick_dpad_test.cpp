#include <cstdio>
#include "../stick_dpad.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
int main() {
    using namespace psrnx;
    StickDpad s;
    CHECK(s.update(0, 0) == 0);
    CHECK(s.update(10000, 0) == 0);                    // below the press threshold
    CHECK(s.update(20000, 0) == kDpadRight);
    CHECK(s.update(14000, 0) == kDpadRight);           // hysteresis: stays pressed above the release threshold
    CHECK(s.update(11000, 0) == 0);
    CHECK(s.update(-30000, 0) == kDpadLeft);
    CHECK(s.update(0, -25000) == kDpadUp);             // SDL Y is down, so negative = up
    CHECK(s.update(0, 25000) == kDpadDown);
    CHECK(s.update(20000, 19000) == kDpadRight);       // diagonal: dominant axis only
    CHECK(s.update(19000, 20000) == kDpadDown);
    CHECK(s.update(0, 0) == 0);
    CHECK(s.update(-32768, 32767) == (kDpadLeft));     // |x| >= |y|: horizontal wins on a tie
    std::printf(failures == 0 ? "STICK OK\n" : "STICK FAILURES\n");
    return failures;
}
