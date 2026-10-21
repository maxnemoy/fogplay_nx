#include "features/streaming/domain/stream_input_gate.hpp"
#include "features/streaming/infrastructure/stream.hpp"
#include <cassert>
#include <iostream>
#include <vector>

namespace {
using fog::stream::StreamInputGate;
// A controller poll supplies the entire current sample, never only button edges.
void sample(StreamInputGate& gate, const stream::Gamepad& current,
            std::vector<stream::Gamepad>& sent, bool overlayHandled = false) {
    if (gate.gamepadAllowed(overlayHandled)) sent.push_back(current);
}
void sameState(const stream::Gamepad& actual, const stream::Gamepad& expected) {
    assert(actual.buttons == expected.buttons && actual.axes == expected.axes);
    assert(actual.leftTrigger == expected.leftTrigger && actual.rightTrigger == expected.rightTrigger);
}
stream::Gamepad heldState() {
    stream::Gamepad value;
    value.buttons[0] = value.buttons[4] = value.buttons[6] = value.buttons[15] = 1;
    value.axes = {0.75f,-0.25f,-0.5f,1.0f};
    value.leftTrigger = 1.0f;
    return value;
}
void heldInputResumes() {
    StreamInputGate gate;
    std::vector<stream::Gamepad> sent;
    const auto held = heldState();
    sample(gate,held,sent);
    gate.suspendVideo();
    sample(gate,held,sent);
    assert(sent.size() == 1 && !gate.suppressOverlay(true,false));
    // An IDR queued for decoding is insufficient; a prepared image is required.
    assert(gate.videoSuspended());
    gate.videoPrepared(true);
    sample(gate,held,sent);
    assert(sent.size() == 1);
    gate.videoPrepared(false);
    sample(gate,held,sent);
    assert(sent.size() == 2);
    sameState(sent.back(),held); // No release/repress or neutral-axis sample needed.
}
void currentSampleWins() {
    StreamInputGate gate;
    std::vector<stream::Gamepad> sent;
    sample(gate,heldState(),sent);
    gate.suspendVideo();
    sample(gate,stream::Gamepad{},sent); // Released while video is unavailable.
    assert(sent.size() == 1);
    gate.videoPrepared(false);
    sample(gate,stream::Gamepad{},sent);
    assert(sent.size() == 2);
    sameState(sent.back(),stream::Gamepad{}); // Never replay the old held state.
    gate.suspendVideo();
    auto changed = heldState();
    changed.buttons[0] = 0; changed.buttons[2] = 1;
    changed.axes = {-1.0f,0.5f,0.0f,-0.75f};
    changed.leftTrigger = 0.0f; changed.rightTrigger = 1.0f;
    gate.videoPrepared(false);
    sample(gate,changed,sent);
    sameState(sent.back(),changed);
}
void realUiStillRequiresRelease() {
    StreamInputGate gate;
    std::vector<stream::Gamepad> sent;
    gate.overlayTransition(); // Start, menu open, menu close share this latch.
    gate.suspendVideo();
    gate.videoPrepared(false);
    assert(gate.suppressOverlay(true,false));
    sample(gate,heldState(),sent);
    assert(sent.empty()); // Recovery never clears the UI latch.
    assert(gate.suppressOverlay(false,true));
    assert(gate.suppressOverlay(false,false)); // Consume release itself.
    assert(!gate.suppressOverlay(true,false));
    sample(gate,heldState(),sent,true); // An open overlay continues to own input.
    assert(sent.empty());
    sample(gate,heldState(),sent);
    assert(sent.size() == 1);
    gate.overlayTransition();
    assert(gate.suppressOverlay(true,false));
    gate.videoPrepared(false);
    assert(gate.suppressOverlay(true,false));
    assert(gate.suppressOverlay(false,false));
    assert(!gate.suppressOverlay(false,false));
}
void nestedAndRepeatedRecovery() {
    StreamInputGate gate;
    std::vector<stream::Gamepad> sent;
    for (int episode = 0; episode < 4; ++episode) {
        gate.suspendVideo(); // Loss detected.
        gate.suspendVideo(); // Decoder reset/generation change in same episode.
        gate.videoPrepared(true); // Another recovery source is still waiting.
        sample(gate,heldState(),sent);
        assert(sent.size() == static_cast<std::size_t>(episode));
        gate.videoPrepared(false);
        sample(gate,heldState(),sent);
        assert(sent.size() == static_cast<std::size_t>(episode+1));
        sameState(sent.back(),heldState());
    }
    gate.overlayTransition(); gate.suspendVideo(); gate.reset();
    assert(!gate.videoSuspended() && !gate.suppressOverlay(true,true));
    assert(gate.gamepadAllowed(false));
    gate.overlayTransition(); // A new stream arms initial anti-clickthrough again.
    assert(!gate.gamepadAllowed(false));
}
}
int main() {
    heldInputResumes(); currentSampleWins(); realUiStillRequiresRelease(); nestedAndRepeatedRecovery();
    std::cout << "Stream input recovery/UI gate tests passed\n";
}
