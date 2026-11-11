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
    assert(schedule.due(state, now + 15ms) == fog::input_options::changeOnly);
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
    if constexpr (fog::input_options::changeOnly) {
        assert(packets > 63 && packets < 250); // Faster real changes, sub-threshold jitter suppressed.
        Schedule quiet; stream::Gamepad neutral;
        quiet.accepted(neutral, now);
        assert(!quiet.due(neutral, now + 199ms));
        assert(quiet.due(neutral, now + 200ms));
        neutral.axes[0] = 0.001f;
        assert(!quiet.due(neutral, now + 1ms));
        neutral.axes[0] = 0.007f;
        assert(quiet.due(neutral, now + 2ms));
        assert(quiet.due(neutral, now + 3ms)); // Failed enqueue remains due.
        quiet.accepted(neutral, now + 3ms);
        neutral.axes[0] = 0.003f;
        assert(!quiet.due(neutral, now + 4ms));
        neutral.axes[0] = 0;
        assert(quiet.due(neutral, now + 5ms)); // Centre must not wait for keepalive.
        quiet.accepted(neutral, now + 5ms);
        assert(!quiet.due(neutral, now + 204ms));
        assert(quiet.due(neutral, now + 205ms));
    } else assert(packets == 63);
    // Press edges count accepted new presses only: holds, releases, axes and
    // rejected (never accepted) samples do not count; reset keeps the count.
    Schedule edges; stream::Gamepad pad;
    edges.accepted(pad, now); assert(edges.pressEdges() == 0);
    pad.axes[0] = 1; edges.accepted(pad, now + 20ms); assert(edges.pressEdges() == 0);
    pad.buttons[1] = 1; assert(edges.pressEdges() == 0); // Not accepted yet.
    edges.accepted(pad, now + 21ms); assert(edges.pressEdges() == 1);
    edges.accepted(pad, now + 40ms); assert(edges.pressEdges() == 1); // Held.
    pad.buttons[2] = 1; edges.accepted(pad, now + 41ms); assert(edges.pressEdges() == 2);
    pad.buttons[1] = pad.buttons[2] = 0; edges.accepted(pad, now + 42ms); assert(edges.pressEdges() == 2);
    edges.reset(); pad.buttons[1] = 1; edges.accepted(pad, now + 43ms); assert(edges.pressEdges() == 3);
    std::cout << "Gamepad schedule: change-only=" << fog::input_options::changeOnly
        << "; edges, accepted-state cadence, jitter/keepalive, neutral retry and press edges PASS\n";
}
