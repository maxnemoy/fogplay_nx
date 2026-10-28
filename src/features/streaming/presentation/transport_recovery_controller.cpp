#include "app/application_controller.hpp"

namespace uiText = ::fog::constants::ui;

namespace fogplay {

bool ApplicationController::transportRecoveryAllowed() const {
    return screen_ == Screen::Streaming && sessionPending_ && !streamRental_.empty() &&
        !stopAfterTask_ && !exitAfterStop_ && !shutdownDone_ && !pending_.valid();
}

void ApplicationController::stopTransportRecovery(const char* reason) {
    // Queue intent even while an asynchronous preflight owns the rental guard.
    // A late result must never restart playback or replenish the retry budget.
    stopAfterTask_ = true;
    if (paired_) paired_->requestStop();
    transportReconnect_.cancel();
    beginFinish(reason);
}

void ApplicationController::retryTransport(const char* reason) {
    if (!transportRecoveryAllowed() || !transportReconnect_.retry(std::chrono::steady_clock::now())) {
        stopTransportRecovery("transport-reconnect-exhausted"); return;
    }
    streamInputGate_.suspendVideo();
    neutralizeStreamInput();
    recordTransportBreak();
    if (!closeTransport(false)) {
        stopTransportRecovery("transport-retry-terminal"); return;
    }
    status_ = uiText::Stream::ReconnectStatus;
    diagnostic("Transport recovery scheduled; reason=" + std::string(reason) +
        " attemptsIn120s=" + std::to_string(transportReconnect_.attempts()) +
        " limit=3 rollingWindow=120s recoveryDeadline=90s");
}

bool ApplicationController::handleTransportState(std::chrono::steady_clock::time_point now) {
    if (!stream_) return true;
    switch (stream_->recoveryState()) {
        case stream::RecoveryState::TerminalFailure:
            stopTransportRecovery("transport-fatal"); return false;
        case stream::RecoveryState::RetryableFailure:
            retryTransport("transport-error"); return false;
        case stream::RecoveryState::Disconnected:
            if (!transportReconnect_.recovering()) {
                transportReconnect_.disconnected(now);
                suspendVideoInput();
                diagnostic("Transport disconnected; waiting up to 5s before replacement; rental remains active.");
            }
            break;
        case stream::RecoveryState::Connected:
            break;
    }
    return true;
}

bool ApplicationController::advanceTransportRecovery(std::chrono::steady_clock::time_point now) {
    switch (transportReconnect_.poll(now)) {
        case fog::stream::TransportReconnect::Action::Stop:
            stopTransportRecovery("transport-reconnect-deadline"); return false;
        case fog::stream::TransportReconnect::Action::CloseTransport:
            retryTransport("transport-recovery-timeout"); return false;
        case fog::stream::TransportReconnect::Action::StartTransport:
            requestTransportRecovery();
            break;
        case fog::stream::TransportReconnect::Action::Continue:
            break;
    }
    return stream_ && !stopAfterTask_;
}

void ApplicationController::requestTransportRecovery() {
    if (!transportRecoveryAllowed() || stream_ || !transportReconnect_.connecting()) {
        stopTransportRecovery("transport-reconnect-unavailable"); return;
    }
    status_ = uiText::Stream::ReconnectStatus;
    diagnostic("Transport reconnect attempt=" + std::to_string(transportReconnect_.attempts()) + "/3 rollingWindow=120s");
    // FH requires a fresh website inspection. MTS revalidates the retained
    // rental through its ordinary signaling handshake. Both return here before
    // the same resume-only start and successful-frame acknowledgement.
    if (streamRental_.provider == fog::ProviderId::FogHub) requestPairedTransportReconnect();
    else completeTransportRecovery(fog::stream::TransportReconnect::Preparation::Ready);
}

void ApplicationController::completeTransportRecovery(fog::stream::TransportReconnect::Preparation preparation) {
    const auto action = transportReconnect_.poll(std::chrono::steady_clock::now());
    if (!transportRecoveryAllowed() || stream_ || !transportReconnect_.connecting() ||
        action != fog::stream::TransportReconnect::Action::Continue) {
        stopTransportRecovery("transport-reconnect-preflight-expired"); return;
    }
    if (preparation == fog::stream::TransportReconnect::Preparation::Retry) {
        retryTransport("transport-preflight-temporarily-unavailable"); return;
    }
    if (preparation != fog::stream::TransportReconnect::Preparation::Ready) {
        stopTransportRecovery("transport-preflight-refused"); return;
    }
    try {
        // Every replacement preserves ownership. Adapters implement the service
        // handshake; resume-only must never launch a new rental/application.
        if (startTransport(true)) {
            status_ = uiText::Stream::ReconnectStatus;
            return;
        }
        if (stream_ && stream_->recoveryState() == stream::RecoveryState::RetryableFailure) {
            retryTransport("transport-retry-start"); return;
        }
    } catch (...) {
        diagnostic("Transport recovery setup failed; stopping the owned rental.");
    }
    stopTransportRecovery("transport-start-fatal");
}

}
