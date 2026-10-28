#include "app/application_controller.hpp"
#include <iostream>

namespace {
using fogplay::ApplicationController;
using Reconnect = fog::stream::TransportReconnect;
using State = stream::RecoveryState;
using namespace std::chrono_literals;
int assertions = 0;
void require(bool value, const char* text) {
    ++assertions;
    if (!value) throw std::runtime_error(text);
}
void provider(ApplicationController& c, fog::ProviderId id) {
    c.streamRental_.provider = id;
    c.pairedPending_ = id == fog::ProviderId::FogHub;
}
void due(ApplicationController& c, Reconnect::Time now = Reconnect::Clock::now()) {
    require(c.transportReconnect_.retry(now - 2s), "retry budget available");
    require(c.transportReconnect_.poll(now) == Reconnect::Action::StartTransport, "resume attempt becomes due");
}
void completePreflight(ApplicationController& c) {
    if (c.streamRental_.provider != fog::ProviderId::FogHub) return;
    require(c.pairedTransportReconnectPending_ && c.pending_.valid(), "FH inspection is asynchronous");
    require(c.starts == 0 && c.paired_->resumeInspections == 0, "host untouched before joined inspection");
    c.joinInspection();
    require(c.completePairedTransportReconnect(), "FH result handled by provider preflight");
}
void inspect(ApplicationController& c) {
    c.stream_.reset(); due(c);
    c.requestTransportRecovery();
    require(c.pairedTransportReconnectPending_ && c.pending_.valid(), "asynchronous inspection dispatched");
    require(c.starts == 0 && c.paired_->resumeInspections == 0, "host untouched before joined inspection");
}
void sharedCycle(fog::ProviderId id) {
    { ApplicationController c; provider(c,id);
      require(c.handleTransportState(Reconnect::Clock::now()) && c.advanceTransportRecovery(Reconnect::Clock::now()) &&
          c.starts == 0 && c.disconnects == 0 && !c.transportReconnect_.recovering(),
          "connected transport stays on the common healthy path"); }
    { ApplicationController c; provider(c,id);
      c.stream_->reportedState = State::RetryableFailure;
      require(!c.handleTransportState(Reconnect::Clock::now()), "classified failure leaves current pump");
      require(!c.stream_ && c.disconnects == 1 && !c.finalDisconnect && c.neutralizations == 1 &&
          c.streamInputGate_.suspensions == 1 && c.transportReconnect_.recovering() && c.finishes == 0,
          "both providers neutralize input and detach on retryable failure");
      require(c.relayBreaks == (id == fog::ProviderId::Mts ? 1 : 0), "relay selection stays MTS-specific"); }
    { ApplicationController c; provider(c,id); const auto now = Reconnect::Clock::now();
      c.stream_->reportedState = State::Disconnected;
      require(c.handleTransportState(now - 5s) && c.transportReconnect_.recovering() && c.disconnects == 0,
          "transient disconnection enters grace without replacing either provider");
      require(c.advanceTransportRecovery(now - 1ms) && c.disconnects == 0,
          "both providers retain the transport until the five-second grace boundary");
      require(!c.advanceTransportRecovery(now) && c.disconnects == 1 && !c.stream_ && c.starts == 0,
          "grace expiration uses the common detach and backoff path");
      require(!c.advanceTransportRecovery(now + 1999ms) && c.starts == 0,
          "both providers wait for two-second backoff");
      c.advanceTransportRecovery(now + 2s); completePreflight(c);
      require(c.starts == 1 && c.resumedOnly && c.startedRental == c.streamRental_ && c.finishes == 0,
          "the same common start resumes the original rental for both providers");
      require(c.transportReconnect_.connecting(), "accepted host start alone does not finish recovery");
      require(c.transportReconnect_.prepared(now + 3s) && !c.transportReconnect_.recovering() &&
          c.transportReconnect_.attempts() == 1, "prepared frame ends recovery while retaining rolling budget"); }
    { ApplicationController c; provider(c,id); const auto now = Reconnect::Clock::now();
      c.stream_->reportedState = State::Disconnected;
      c.handleTransportState(now - 1s);
      c.stream_->reportedState = State::Connected;
      require(c.handleTransportState(now) && c.advanceTransportRecovery(now) && c.starts == 0 && c.disconnects == 0,
          "either adapter may recover during grace on its existing transport");
      require(c.transportReconnect_.prepared(now), "a new prepared frame can acknowledge grace recovery"); }
    { ApplicationController c; provider(c,id); const auto now = Reconnect::Clock::now();
      c.stream_->reportedState = State::Disconnected; c.handleTransportState(now - 5s);
      c.stream_->reportedState = State::TerminalFailure;
      require(!c.handleTransportState(now) && !c.advanceTransportRecovery(now) && c.finishes == 1 &&
          c.starts == 0 && c.disconnects == 0, "terminal evidence wins over a due retry for both providers"); }
    { ApplicationController c; provider(c,id); c.stream_.reset(); const auto now = Reconnect::Clock::now();
      for (int i = 0; i < 3; ++i) {
          due(c, now - std::chrono::seconds(8 - i * 3));
          require(c.transportReconnect_.prepared(now - std::chrono::seconds(7 - i * 3)), "synthetic successful frame");
      }
      c.retryTransport("fourth-loss");
      require(c.finishes == 1 && c.starts == 0, "three attempts in rolling window cap either provider"); }
    { ApplicationController c; provider(c,id); c.stream_.reset(); const auto now = Reconnect::Clock::now();
      c.transportReconnect_.disconnected(now - 90s);
      require(!c.advanceTransportRecovery(now) && c.finishes == 1 && c.starts == 0,
          "ninety-second recovery deadline stops either provider"); }
    { ApplicationController c; provider(c,id); c.disconnectAllowed = false; c.retryTransport("terminal-raced-close");
      require(c.finishes == 1 && c.starts == 0, "either provider refuses retry after terminal evidence during detach"); }
    { ApplicationController c; provider(c,id); c.startAllowed = false; c.startRetryable = true;
      c.stream_.reset(); due(c); c.requestTransportRecovery(); completePreflight(c);
      require(c.starts == 1 && c.finishes == 0 && !c.stream_ && c.transportReconnect_.recovering() &&
          !c.transportReconnect_.connecting(), "classified setup failure uses the common retry path"); }
    for (int stop = 0; stop < 3; ++stop) {
        ApplicationController c; provider(c,id); c.stream_.reset(); due(c);
        if (stop == 0) c.stopAfterTask_ = true;
        if (stop == 1) c.exitAfterStop_ = true;
        if (stop == 2) c.shutdownDone_ = true;
        c.requestTransportRecovery();
        require(c.starts == 0 && !c.pending_.valid() && c.finishes == 1,
            "user exit or shutdown prevents a due start for both providers");
    }
}
}

int main() {
    try {
        sharedCycle(fog::ProviderId::Mts);
        sharedCycle(fog::ProviderId::FogHub);
        { ApplicationController c; inspect(c); c.joinInspection();
          require(c.completePairedTransportReconnect() && c.starts == 1 && c.resumedOnly &&
              c.startedRental == c.streamRental_ && c.finishes == 0,
              "fresh FH verification enters the common resume-only start"); }
        { ApplicationController c; inspect(c); c.joinInspection();
          c.paired_->value.state = fog::PairedCheckoutState::Preparing; c.paired_->value.endpoint.reset();
          c.completePairedTransportReconnect();
          require(c.finishes == 0 && c.starts == 0 && c.transportReconnect_.recovering() &&
              !c.transportReconnect_.connecting(), "temporary inspection failure returns to common backoff"); }
        for (int failure = 0; failure < 8; ++failure) {
            ApplicationController c; inspect(c); c.joinInspection();
            if (failure == 0) c.stopAfterTask_ = true;
            if (failure == 1) c.exitAfterStop_ = true;
            if (failure == 2) c.shutdownDone_ = true;
            if (failure == 3) c.paired_->value.rental.value = "different";
            if (failure == 4) c.paired_->value.endpoint.reset();
            if (failure == 5) c.paired_->value.stopRequested = true;
            if (failure == 6) c.paired_->value.state = fog::PairedCheckoutState::RecoveryRequired;
            if (failure == 7) c.screen_ = fogplay::Screen::Recovery;
            c.completePairedTransportReconnect();
            require(c.starts == 0 && c.finishes == 1, "stop intent or invalid FH verification never resumes");
        }
        { ApplicationController c; inspect(c); c.joinInspection();
          c.pairedPending_ = c.sessionPending_ = false;
          require(!c.completePairedTransportReconnect() && c.streamCloses == 1 && c.starts == 0 && c.finishes == 0,
              "confirmed terminal result returns to ordinary terminal handling without resume"); }
        { ApplicationController c; c.stream_.reset(); const auto now = Reconnect::Clock::now();
          due(c,now - 31s); c.requestTransportRecovery();
          require(c.pending_.valid(), "slow inspection owns guard before timeout");
          require(!c.advanceTransportRecovery(now) && c.stopAfterTask_ && c.paired_->stopRequests == 1 &&
              !c.transportReconnect_.recovering() && c.finishes == 0,
              "production thirty-second timeout queues stop while inspection owns guard");
          c.joinInspection(); c.completePairedTransportReconnect();
          require(c.starts == 0 && c.finishes == 1, "late ready inspection cannot undo queued timeout stop"); }
        { ApplicationController c; c.stream_.reset(); due(c, Reconnect::Clock::now() - 31s);
          c.requestTransportRecovery(); c.joinInspection(); c.completePairedTransportReconnect();
          require(c.starts == 0 && c.finishes == 1, "late inspection completion checks deadline even before next pump"); }
        for (int failure = 0; failure < 3; ++failure) {
            ApplicationController c; c.stream_.reset(); due(c);
            if (failure == 0) c.dispatchThrows = true;
            c.requestTransportRecovery();
            if (failure != 0) {
                c.joinInspection();
                if (failure == 1) c.startAllowed = false;
                if (failure == 2) c.startThrows = true;
                c.completePairedTransportReconnect();
            }
            require(c.finishes == 1 && !c.pairedTransportReconnectPending_, "dispatch/setup failure uses owned rental cleanup");
        }
        std::cout << "Shared MTS/FH transport recovery: " << assertions << " assertions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Shared transport recovery failed: " << error.what() << '\n'; return 1;
    }
}
