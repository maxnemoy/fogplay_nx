#include "features/streaming/domain/video_recovery.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
using namespace std::chrono_literals;
int main() {
    fog::VideoRecovery r;
    const auto t=fog::VideoRecovery::Time{};
    assert(!r.active() && !r.expired(t+100s));
    assert(r.begin(t));
    assert(r.active() && !r.expired(t+9999ms));
    assert(r.begin(t+9s));
    assert(r.expired(t+10s)); // Second error must not buy another ten seconds.
    assert(!r.begin(t+10s));
    r.reset();
    for (int i=0;i<3;++i) { assert(r.begin(t+i*1s)); r.decoded(); }
    assert(!r.begin(t+3s)); // Isolated good frames do not erase failure history.
    assert(r.begin(t+60s)); // Rolling minute releases the oldest slot.
    assert(r.recentResets()==3);
    r.decoded();
    assert(!r.begin(t+60s));
    assert(r.begin(t+61s));
    r.reset();
    assert(!r.active() && r.recentResets()==0 && r.begin(t));
    r.decoded();
    assert(!r.expired(t+20s));
    std::cout << "Video recovery deadline/budget tests passed\n";
}
