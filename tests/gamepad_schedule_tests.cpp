#include "features/streaming/domain/gamepad_schedule.hpp"
#include <cassert>
#include <iostream>
#include <vector>

int main() {
    using Schedule = fog::stream::GamepadSchedule;
    using namespace std::chrono_literals;
    const auto now = Schedule::Time{};
    Schedule schedule;
    stream::Gamepad state;
    assert(schedule.due(state, now)); schedule.accepted(state, now);
    state.axes[0] = 0.5f;
    assert(!schedule.due(state, now + 15ms));
    assert(schedule.due(state, now + 16ms)); schedule.accepted(state, now + 16ms);
    // A complete short tap fits inside the analog interval, for every button.
    for (std::size_t i = 0; i < state.buttons.size(); ++i) {
        const auto at = now + 20ms + i * 3ms;
        state.buttons[i] = 1;
        assert(schedule.due(state, at));
        assert(schedule.due(state, at + 1ms)); // Failed send did not advance accepted state.
        schedule.accepted(state, at + 1ms);
        state.buttons[i] = 0;
        assert(schedule.due(state, at + 2ms)); schedule.accepted(state, at + 2ms);
    }
    state.leftTrigger = 1;
    assert(schedule.due(state, now + 80ms)); schedule.accepted(state, now + 80ms);
    state.leftTrigger = 0; state.rightTrigger = 1;
    assert(schedule.due(state, now + 81ms)); schedule.accepted(state, now + 81ms);
    state.rightTrigger = 0;
    assert(schedule.due(state, now + 82ms)); schedule.accepted(state, now + 82ms);
    // Menu/video suspension retries a rejected neutral before any fresh input.
    state.buttons[0] = 1; schedule.accepted(state, now + 83ms);
    unsigned rejected = 0;
    schedule.suspend([&](const auto& neutral) { ++rejected; assert(neutral.buttons[0] == 0); return false; });
    assert(!schedule.due(state, now + 100ms));
    schedule.retryRelease([&](const auto&) { ++rejected; return false; });
    assert(!schedule.due(state, now + 110ms) && rejected == 2);
    unsigned releases = 0;
    schedule.retryRelease([&](const auto& neutral) { ++releases; assert(neutral.axes[0] == 0); return true; });
    schedule.retryRelease([&](const auto&) { ++releases; return true; });
    assert(releases == 1 && schedule.due(state, now + 111ms));
    // Teardown drops old retries; a new transport starts with its current sample.
    schedule.suspend([](const auto&) { return false; });
    schedule.reset();
    schedule.retryRelease([&](const auto&) { ++releases; return true; });
    state = {};
    assert(releases == 1 && schedule.due(state, now + 112ms));
    schedule.accepted(state, now + 112ms);
    assert(!schedule.due(state, now + 113ms));
    assert(schedule.due(state, now)); // A rewound test clock cannot wedge input.
    // Cadence follows actual acceptance, not failed calls or packet backlog.
    schedule.reset(); unsigned packets = 0;
    for (unsigned ms = 0; ms < 1000; ++ms) {
        state.axes[1] = static_cast<float>(ms) / 1000;
        if (schedule.due(state, now + std::chrono::milliseconds(ms))) {
            ++packets; schedule.accepted(state, now + std::chrono::milliseconds(ms));
        }
    }
    assert(packets == 63);
    std::cout << "Gamepad schedule: immediate button/trigger edges, 16ms analog, rejected sends, menu neutral retry and fresh transport PASS\n";
}
