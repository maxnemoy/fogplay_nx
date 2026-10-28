#include "features/streaming/domain/mouse_buttons.hpp"
#include <cassert>
#include <iostream>
#include <utility>
#include <vector>

namespace {
using Buttons = fog::stream::MouseButtons;
using Event = std::pair<bool, int>;
struct Transport {
    bool available = true;
    int rejected = 0;
    std::vector<Event> attempts, accepted;
    bool operator()(bool down, int button) {
        attempts.emplace_back(down,button);
        if (!available) return false;
        if (rejected) { --rejected; return false; }
        accepted.emplace_back(down,button);
        return true;
    }
};

void failedDownUsesCurrentSample() {
    Buttons buttons;
    Transport transport;
    transport.rejected = 1;
    buttons.update(true,false,transport);
    assert(buttons.differs(true,false) && transport.accepted.empty());
    buttons.update(true,false,transport);
    assert(!buttons.differs(true,false));
    assert((transport.accepted == std::vector<Event>{{true,0}}));
    buttons.update(false,false,transport);
    assert((transport.accepted == std::vector<Event>{{true,0},{false,0}}));

    transport.rejected = 1;
    buttons.update(true,false,transport);
    const auto attempts = transport.attempts.size();
    buttons.update(false,false,transport);
    buttons.retryReleases(transport);
    assert(transport.attempts.size() == attempts); // No delayed click or redundant up.
    assert(!buttons.differs(false,false));
}

void failedUpRetriesOutsideMouseMode() {
    Buttons buttons;
    Transport transport;
    buttons.update(true,true,transport);
    transport.rejected = 2;
    buttons.update(false,false,transport);
    assert(buttons.differs(false,false));
    // Opening the menu clears physical drag intent, but cannot erase rejected ups.
    buttons.releaseAll();
    buttons.retryReleases(transport);
    assert(!buttons.differs(false,false));
    assert((transport.accepted == std::vector<Event>{{true,0},{true,2},{false,0},{false,2}}));
    const auto attempts = transport.attempts.size();
    buttons.releaseAll();
    buttons.retryReleases(transport);
    assert(transport.attempts.size() == attempts); // A completed release is not repeated.
}

void menuReleaseSurvivesTemporaryChannelFailure() {
    Buttons buttons;
    Transport transport;
    buttons.update(true,true,transport);
    buttons.releaseAll();
    transport.available = false;
    buttons.retryReleases(transport);
    buttons.retryReleases(transport);
    assert(buttons.differs(false,false));
    transport.available = true;
    buttons.retryReleases(transport);
    assert(!buttons.differs(false,false));
    assert((transport.accepted == std::vector<Event>{{true,0},{true,2},{false,0},{false,2}}));
}

void pendingUpFinishesBeforeAnotherPress() {
    Buttons buttons;
    Transport transport;
    buttons.update(true,false,transport);
    transport.rejected = 1;
    buttons.update(false,false,transport);
    buttons.update(true,false,transport);
    assert((transport.accepted == std::vector<Event>{{true,0},{false,0}}));
    assert(buttons.differs(true,false));
    buttons.update(true,false,transport);
    assert((transport.accepted == std::vector<Event>{{true,0},{false,0},{true,0}}));
}

void detachedTransportDropsOldState() {
    Buttons buttons;
    Transport oldTransport;
    buttons.update(true,true,oldTransport);
    buttons.releaseAll();
    oldTransport.available = false;
    buttons.retryReleases(oldTransport);
    buttons.reset(); // Only after the owning old transport is detached.
    Transport nextTransport;
    buttons.retryReleases(nextTransport);
    buttons.update(false,false,nextTransport);
    assert(nextTransport.attempts.empty());
    buttons.update(false,true,nextTransport);
    assert((nextTransport.accepted == std::vector<Event>{{true,2}}));
}
}

int main() {
    failedDownUsesCurrentSample();
    failedUpRetriesOutsideMouseMode();
    menuReleaseSurvivesTemporaryChannelFailure();
    pendingUpFinishesBeforeAnotherPress();
    detachedTransportDropsOldState();
    std::cout << "Mouse button enqueue/release recovery tests passed\n";
}
