#include "features/streaming/domain/stick_deadzone.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

namespace {
float length(fog::stream::StickAxes value) { return std::hypot(value.x,value.y); }
bool near(float a, float b) { return std::abs(a-b) < 0.0001f; }
}
int main() {
    using fog::stream::radialStick;
    assert(length(radialStick(0,0)) == 0);
    assert(length(radialStick(1310,0)) == 0); // Below the 4% radius.
    const auto diagonal = radialStick(1000,1000);
    assert(diagonal.x > 0 && near(diagonal.x,diagonal.y)); // Not a square threshold.
    const auto edge = radialStick(1311,0);
    assert(edge.x > 0 && edge.x < 0.0001f && edge.y == 0); // Continuous boundary.
    const auto small = radialStick(2000,0), large = radialStick(4000,0);
    assert(small.x > 0 && large.x > small.x);
    assert(near(radialStick(32767,0).x,1));
    assert(near(radialStick(-32768,0).x,-1));
    const auto saturated = radialStick(32767,-32768);
    assert(near(length(saturated),1) && near(saturated.x,-saturated.y));
    const auto direction = radialStick(8000,-4000);
    assert(near(direction.x / direction.y,-2));
    assert(near(radialStick(-8000,4000).x,-direction.x));
    assert(near(length(radialStick(100000,100000)),1));
    const auto gameDrift = radialStick(2200,2200);
    const auto cursorDrift = fog::stream::cursorStick(2200,2200);
    assert(length(gameDrift)>0 && length(cursorDrift)==0); // 6.7% Joy-Con drift.
    assert(length(fog::stream::cursorStick(3932,0))==0); // Below 12%, independent of C2.
    assert(fog::stream::cursorStick(4000,0).x>0);
    std::cout << "Radial sticks: 4% radius, continuous remap, direction and saturation PASS\n";
}
