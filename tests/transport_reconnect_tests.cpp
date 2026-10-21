#include "features/streaming/domain/transport_reconnect.hpp"
#include "features/streaming/domain/stream_input_gate.hpp"
#include <cassert>
#include <chrono>
#include <iostream>

namespace {
using Policy = fog::stream::TransportReconnect;
using Action = Policy::Action;
using namespace std::chrono_literals;
const Policy::Time zero{};
void graceAndSpontaneousRecovery() {
    Policy policy;
    assert(policy.poll(zero) == Action::Continue && !policy.recovering());
    assert(!policy.prepared(zero));
    assert(policy.disconnected(zero));
    assert(policy.poll(zero+4999ms) == Action::Continue);
    // A repeated disconnected observation does not extend grace.
    assert(policy.disconnected(zero+4s));
    assert(policy.poll(zero+5s) == Action::CloseTransport);
    assert(!policy.prepared(zero+5s)); // No old image may end backoff.
    assert(policy.poll(zero+6999ms) == Action::Continue);
    assert(policy.poll(zero+7s) == Action::StartTransport && policy.attempts() == 1);
    assert(policy.prepared(zero+8s));
    assert(!policy.recovering() && policy.attempts() == 1);
    // Short natural loss does not consume another replacement attempt.
    assert(policy.disconnected(zero+10s));
    assert(policy.prepared(zero+14s));
    assert(policy.attempts() == 1 && policy.poll(zero+50s) == Action::Continue);
}
void initialFailureAndAttemptDeadline() {
    Policy policy;
    assert(policy.retry(zero)); // Initial transport fails before its first frame.
    assert(policy.retry(zero+1s)); // Duplicate failure keeps the original backoff.
    assert(policy.poll(zero+1999ms) == Action::Continue);
    assert(policy.poll(zero+2s) == Action::StartTransport && policy.connecting());
    assert(policy.poll(zero+31999ms) == Action::Continue);
    assert(!policy.prepared(zero+32s)); // Boundary is an expired attempt.
    assert(policy.poll(zero+32s) == Action::CloseTransport);
    assert(policy.poll(zero+33999ms) == Action::Continue);
    assert(policy.poll(zero+34s) == Action::StartTransport && policy.attempts() == 2);
    assert(policy.prepared(zero+35s));
}
void lifetimeBudgetSurvivesBriefSuccess() {
    Policy policy;
    for (unsigned attempt = 1; attempt <= Policy::MaxAttempts; ++attempt) {
        const auto began = zero+std::chrono::seconds(attempt*100);
        assert(policy.retry(began));
        assert(policy.poll(began+2s) == Action::StartTransport);
        assert(policy.attempts() == attempt);
        assert(policy.prepared(began+3s));
        assert(policy.attempts() == attempt);
    }
    assert(!policy.retry(zero+400s));
    assert(policy.poll(zero+401s) == Action::Stop);
    assert(!policy.prepared(zero+401s));
    policy.reset(); // Only the final rental close/new rental resets the budget.
    assert(policy.attempts() == 0 && policy.retry(zero+500s));
    assert(policy.poll(zero+502s) == Action::StartTransport);
}
void thirdFailedAttemptStops() {
    Policy policy;
    assert(policy.retry(zero));
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        const auto began = zero+std::chrono::seconds(attempt*2);
        assert(policy.poll(began) == Action::StartTransport);
        assert(policy.attempts() == attempt);
        if (attempt < 3) assert(policy.retry(began));
        else assert(!policy.retry(began));
    }
    assert(policy.poll(zero+7s) == Action::Stop);
}
void totalWindowCapIncludesGraceAndBackoff() {
    Policy policy;
    assert(policy.disconnected(zero));
    assert(policy.poll(zero+5s) == Action::CloseTransport);
    assert(policy.poll(zero+7s) == Action::StartTransport);
    assert(policy.poll(zero+37s) == Action::CloseTransport);
    assert(policy.poll(zero+39s) == Action::StartTransport);
    assert(policy.poll(zero+69s) == Action::CloseTransport);
    assert(policy.poll(zero+71s) == Action::StartTransport);
    assert(policy.poll(zero+89999ms) == Action::Continue);
    assert(!policy.prepared(zero+90s));
    assert(policy.poll(zero+90s) == Action::Stop); // Third attempt truncated by total cap.
    assert(!policy.retry(zero+90s));
    Policy stalled;
    assert(stalled.retry(zero));
    assert(stalled.poll(zero+90s) == Action::Stop); // Main thread stall cannot launch late.
    assert(stalled.attempts() == 0);
}
void successStartsANewWindowButNotANewBudget() {
    Policy policy;
    assert(policy.retry(zero));
    assert(policy.poll(zero+2s) == Action::StartTransport);
    assert(policy.prepared(zero+3s));
    assert(policy.retry(zero+1000s));
    assert(policy.poll(zero+1002s) == Action::StartTransport);
    assert(policy.attempts() == 2 && policy.prepared(zero+1003s));
}
void cancellationWinsInEveryPhase() {
    for (int phase = 0; phase < 4; ++phase) {
        Policy policy;
        if (phase == 1) policy.disconnected(zero);
        if (phase >= 2) policy.retry(zero);
        if (phase == 3) assert(policy.poll(zero+2s) == Action::StartTransport);
        const auto used = policy.attempts();
        policy.cancel();
        assert(!policy.recovering() && !policy.prepared(zero+3s));
        assert(!policy.retry(zero+3s) && !policy.disconnected(zero+3s));
        assert(policy.poll(zero+1000s) == Action::Continue);
        assert(policy.attempts() == used);
    }
}
void inputResumesFreshWithoutUiRelease() {
    Policy policy;
    fog::stream::StreamInputGate gate;
    gate.suspendVideo();
    assert(policy.retry(zero));
    assert(policy.poll(zero+2s) == Action::StartTransport);
    gate.videoPrepared(policy.recovering());
    assert(!gate.gamepadAllowed(false));
    assert(policy.prepared(zero+3s));
    gate.videoPrepared(policy.recovering());
    assert(gate.gamepadAllowed(false)); // No menu-transition latch introduced by retry.
    gate.overlayTransition();
    gate.suspendVideo();
    gate.videoPrepared(false);
    assert(!gate.gamepadAllowed(false)); // A real open menu still owns the sample.
}
}
int main() {
    graceAndSpontaneousRecovery(); initialFailureAndAttemptDeadline();
    lifetimeBudgetSurvivesBriefSuccess(); thirdFailedAttemptStops();
    totalWindowCapIncludesGraceAndBackoff(); successStartsANewWindowButNotANewBudget();
    cancellationWinsInEveryPhase(); inputResumesFreshWithoutUiRelease();
    std::cout << "Same-rental reconnect deadline, budget, cancellation and input tests passed\n";
}
