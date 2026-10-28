#include "app/application_controller.hpp"

namespace fogplay {

void ApplicationController::requestPairedTransportReconnect() {
    // Provider-specific preflight only. Scheduling, cancellation and host start
    // belong to the common transport recovery controller.
    if (!pairedPending_ || !paired_ || !pairedServices_) {
        completeTransportRecovery(fog::stream::TransportReconnect::Preparation::Refused);
        return;
    }
    pairedTransportReconnectPending_ = true;
    try {
        start(Task::PairedPoll,[this] { paired_->inspectForResume(); return std::monostate{}; });
    } catch (...) {
        pairedTransportReconnectPending_ = false;
        stopTransportRecovery("reconnect-inspection-dispatch-failed");
        return;
    }
    diagnostic("FH recovery preflight: checking original account, exact rental and paired endpoint.");
}

bool ApplicationController::completePairedTransportReconnect() {
    if (!pairedTransportReconnectPending_) return false;
    pairedTransportReconnectPending_ = false;
    if (!pairedPending_) {
        closeStream();
        return false; // Ordinary joined terminal-result handling owns the UI.
    }
    using Preparation = fog::stream::TransportReconnect::Preparation;
    const auto& result = paired_->result();
    auto preparation = Preparation::Refused;
    if (!result.stopRequested && result.rental == streamRental_) {
        if (result.state == fog::PairedCheckoutState::Ready && result.endpoint)
            preparation = Preparation::Ready;
        else if (result.state == fog::PairedCheckoutState::Preparing)
            preparation = Preparation::Retry;
    }
    completeTransportRecovery(preparation);
    return true;
}

}
