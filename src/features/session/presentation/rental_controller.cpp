#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"

namespace uiText = ::fog::constants::ui;

namespace fogplay {

void ApplicationController::armExitHandling() {
    checkResult(appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend), "Background stop support");
    if (!exitLocked_) { checkResult(appletLockExit(), "Session exit guard"); exitLocked_ = true; }
    checkResult(appletSetAutoSleepDisabled(true), "Session autosleep guard");
}

void ApplicationController::disarmExitHandling() {
    if (exitLocked_) { appletUnlockExit(); exitLocked_ = false; }
    appletSetAutoSleepDisabled(false);
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
}

void ApplicationController::shutdownSession() noexcept {
    if (shutdownDone_) return;
    shutdownDone_ = true;
    diagnostic("Application shutdown: checking owned rental cleanup.");
    // Dispatch the owned rental stop before transport shutdown can wait for an
    // in-flight control send. Its worker uses checkout/guard, not the transport.
    if (sessionPending_ && !pending_.valid()) {
        try { beginFinish("application-shutdown"); }
        catch (...) { diagnostic("Shutdown stop dispatch failed; recovery records retained."); }
    }
    closeStream();
    // Join any owner of checkout/guard before inspecting them. Do not permanently
    // cancel API requests yet: an unresolved order can still need its hash polled.
    if (!sessionPending_ && task_ != Task::Create) services_->cancel();
    if (pending_.valid()) {
        try { pending_.get(); } catch (...) {}
    }
    syncPending();
    PadState shutdownPad;
    padInitializeDefault(&shutdownPad);
    bool exitChordReleased = false;
    std::chrono::steady_clock::time_point exitChordAt{};
    bool manualExit = false;
    bool first = true;
    auto retry = std::chrono::steady_clock::now();
    while (sessionPending_) {
        appletMainLoop(); // OS exit grants only a bounded grace period, not immunity.
        if (std::chrono::steady_clock::now() >= retry) {
            if (first) {
                first = false;
                try { armExitHandling(); }
                catch (...) { diagnostic("Cannot re-arm exit guard; still attempting rental stop."); }
            }
            try {
                if (checkout_.unresolved()) checkout_.stop();
                else if (auth_.authenticated()) {
                    if (!guard_.ownsSession()) guard_.recoverMatchingSession();
                    if (guard_.ownsSession()) guard_.finishAttempt();
                }
                syncPending();
            } catch (...) { diagnostic("Shutdown stop attempt failed; recovery records retained."); }
            if (!sessionPending_) break;
            diagnostic("SESSION STOP UNCONFIRMED: retaining network and recovery records; retry pending.");
            retry = std::chrono::steady_clock::now() + std::chrono::seconds(exitAfterStop_ ? 1 : 10);
        }
        if (appletGetFocusState() == AppletFocusState_InFocus) {
            padUpdate(&shutdownPad);
            const auto buttons = padGetButtons(&shutdownPad);
            const auto chord = HidNpadButton_Plus | HidNpadButton_Minus;
            if ((buttons & chord) != chord) { exitChordReleased = true; exitChordAt = {}; }
            else if (exitChordReleased) {
                if (exitChordAt == std::chrono::steady_clock::time_point{}) exitChordAt = std::chrono::steady_clock::now();
                if (std::chrono::steady_clock::now() - exitChordAt >= std::chrono::seconds(3)) { manualExit = true; break; }
            }
            try {
                SDL_SetRenderDrawColor(rt_.renderer, 12, 16, 28, 255);
                SDL_RenderClear(rt_.renderer);
                ui_.text(uiText::Server::TerminationUnconfirmed, 40, 240, 1200);
                ui_.text(uiText::Server::StopRetryNotice, 40, 290, 1200);
                ui_.text(uiText::Server::ManualExitHint, 40, 340, 1200);
                ui_.text(uiText::Server::ManualExitWarning, 40, 390, 1200);
                SDL_RenderPresent(rt_.renderer);
            } catch (...) {}
        }
        svcSleepThread(100000000);
    }
    diagnostic(manualExit ? "Shutdown: explicit manual exit with unresolved rental; recovery records retained." :
        "Shutdown: no unresolved rental remains.");
    services_->cancel();
    disarmExitHandling();
}

void ApplicationController::syncPending() {
    sessionPending_ = checkout_.unresolved() || guard_.persistent() ||
        !guard_.pendingRental().empty() || !guard_.journalError().empty();
}

void ApplicationController::logCheckoutResult(Task phase) {
    // Only called after get() joins the worker. Log presence/counters, never identifiers.
    const auto& result = checkout_.result();
    diagnostic("Checkout phase=" + std::to_string(static_cast<int>(phase)) +
        " state=" + std::to_string(static_cast<int>(result.state)) +
        " serverStatus=" + std::to_string(result.serverStatus) +
        " orderPresent=" + std::to_string(!result.creationId.empty()) +
        " hashPresent=" + std::to_string(!result.rentalId.value.empty()) +
        " serverHashPresent=" + std::to_string(result.serverHashPresent) +
        " stopRequested=" + std::to_string(result.stopRequested) +
        " stopQueued=" + std::to_string(stopAfterTask_) +
        " pollCount=" + std::to_string(result.pollCount) +
        " readyObservations=" + std::to_string(result.readyObservations));
    diagnostic("Checkout detail: " + result.message);
}

void ApplicationController::queueCheckoutStop() {
    diagnostic("Stop trigger: user pressed Back during checkout.");
    stopAfterTask_ = true;
    checkoutStatus_ = uiText::Server::StopAfterResponse;
    status_ = checkoutStatus_;
    if (!pending_.valid()) beginFinish();
}

void ApplicationController::handleCheckout() {
    syncPending();
    const auto& result = checkout_.result();
    checkoutAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    checkoutAutoPoll_ = fog::checkoutNeedsAutomaticCheck(result);
    if (result.stopRequested) checkoutAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(exitAfterStop_ ? 1 : 10);
    if (result.state == fog::CheckoutState::Ready && !stopAfterTask_ && !result.stopRequested) {
        streamPlayStation_ = selectedPlayStation_;
        startStream(result.rentalId,!selectedPlayStation_); return;
    }
    if (!sessionPending_) {
        disarmExitHandling(); stopAfterTask_ = false;
        screen_ = selectedPlayStation_ ? Screen::PlayStation : Screen::Games;
        status_ = result.state == fog::CheckoutState::Rejected ? uiText::Server::OrderRejected : uiText::Server::RentalFinished;
        return;
    }
    screen_ = Screen::Checkout;
    if (result.stopRequested || stopAfterTask_) checkoutStatus_ = uiText::Server::WaitingStopData;
    else if (result.state == fog::CheckoutState::RecoveryRequired) checkoutStatus_ = ui_.controllerHints(uiText::Server::OrderRecoveryHint);
    else if (result.pollCount >= 30) checkoutStatus_ = ui_.controllerHints(uiText::Server::AutoCheckPausedHint);
    else if (result.readyObservations > 0) checkoutStatus_ = uiText::Server::ReadyObservationsPrefix + std::to_string(result.readyObservations) + "/3)…";
    else if (result.preparation == fog::PreparationState::PaymentConfirmed && !result.serverHashPresent) checkoutStatus_ = uiText::Server::PaymentConfirmedWaitingAddress;
    else if (result.preparation == fog::PreparationState::Preparing) checkoutStatus_ = result.serverHashPresent ? uiText::Server::PreparingConnection : uiText::Server::WaitingAddress;
    else if (result.preparation == fog::PreparationState::Pending) checkoutStatus_ = uiText::Server::OrderCreated;
    else if (result.state == fog::CheckoutState::PaymentRequired) checkoutStatus_ = uiText::Server::PaymentRequired;
    else checkoutStatus_ = uiText::Server::OrderRecorded;
    status_ = checkoutStatus_;
    if (!result.stopRequested && (stopAfterTask_ ||
        (result.state == fog::CheckoutState::RecoveryRequired && !result.rentalId.value.empty()))) {
        diagnostic("Stop trigger: checkout requires recovery after session identification.");
        beginFinish();
    }
}

void ApplicationController::pollCheckout(bool manual) {
    if (pending_.valid()) return;
    start(Task::CheckoutPoll, [this, manual] { checkout_.poll(manual); return std::monostate{}; });
}

void ApplicationController::createSession() {
    const bool playStation = screen_ == Screen::PlayStationDetails;
    if (screen_ != Screen::Details && !playStation) return;
    if (playStation && (playStationLoadedAt_ == std::chrono::steady_clock::time_point{} ||
        std::chrono::steady_clock::now()-playStationLoadedAt_ > std::chrono::seconds(60))) {
        loadPlayStation();
        status_ = uiText::Server::RefreshingTariff;
        return;
    }
    const auto& computers = playStation ? playStationComputers_ : computers_;
    if (sessionPending_ || selection_ >= computers.size()) return;
    const auto& pc = computers[selection_];
    fog::StartRentalRequest order;
    order.computerId = pc.id;
    order.offerId = pc.offerId;
    if (playStation && (!pc.listed || !pc.launchable ||
        !fog::isPlayStationComputerId(order.computerId) || order.offerId.empty())) {
        status_ = uiText::PlayStation::TariffUnconfirmedHint;
        return;
    }
    if (!auth_.authenticated()) {
        loginReturn_ = playStation ? Screen::PlayStationDetails : Screen::Details;
        screen_ = Screen::Consent; return;
    }
    // Verify the same SD replacement path before any billable request.
    checkStorage();
    selectedPlayStation_ = playStation;
    order.gameId = playStation ? fog::EntityId{} : selectedGameId_;
    order.ownAccount = playStation ? false : ownSteam_;
    recentLaunch_.prepare(playStation ? std::nullopt : selectedRecentGame_);
    // Lock focus/exit before the worker can send a billable create request.
    armExitHandling(); sessionPending_ = true; stopAfterTask_ = false;
    screen_ = Screen::Checkout;
    checkoutStatus_ = playStation ? uiText::PlayStation::CheckingTariff : uiText::Server::CreatingOrder;
    start(Task::Create, [this, order, playStation, offeredComputer = pc]() -> TaskResult {
        if (playStation) {
            auto current = catalogRepository_.playStationComputers();
            const auto offer = std::find_if(current.begin(),current.end(),[&order](const fog::Computer& candidate) {
                return candidate.id == order.computerId;
            });
            if (offer == current.end() || !offer->listed || !offer->launchable ||
                !fog::sameOffer(*offer,offeredComputer)) {
                return ChangedOffer{std::move(current)};
            }
            // Main-thread lifecycle checks must accept this result before billing.
            return VerifiedOffer{order};
        }
        checkout_.create(order);
        return std::monostate{};
    });
}

void ApplicationController::loadSessions() {
    if (!auth_.authenticated()) { screen_ = Screen::Consent; return; }
    if (sessionPending_) { recoverSession(); return; }
    screen_ = Screen::Sessions; selection_ = 0; sessions_.clear();
    start(Task::Sessions, [this] { return rentals_.activeSessions(); });
}

void ApplicationController::recoverSession() {
    if (pending_.valid()) return;
    screen_ = Screen::Recovery;
    if (checkout_.unresolved()) { armExitHandling(); stopAfterTask_ = true; beginFinish(); return; }
    start(Task::Recovery, [this] { return RecoveryResult{guard_.recoverMatchingSession()}; });
}

void ApplicationController::connectSession() {
    if (selection_ >= sessions_.size() || sessionPending_) return;
    const auto rental = sessions_[selection_].id;
    // Durable ownership must succeed before any signaling / media operation.
    try { guard_.adopt(rental); }
    catch (...) {
        sessionPending_ = !guard_.pendingRental().empty() || guard_.persistent() || !guard_.journalError().empty();
        if (sessionPending_) {
            screen_ = Screen::Recovery;
            if (guard_.ownsSession()) beginFinish();
        }
        throw;
    }
    // Only an exact known computer identity selects the PlayStation protocol.
    // Missing/unknown identity keeps the ordinary stream mode.
    const auto& session = sessions_[selection_];
    streamPlayStation_ = session.computerId && fog::isPlayStationComputerId(*session.computerId);
    startStream(rental,false);
}

void ApplicationController::beginFinish(const char* reason) {
    if (pending_.valid() || !sessionPending_) return;
    diagnostic("Rental stop begin; reason=" + std::string(reason) + " screen=" + std::to_string(static_cast<int>(screen_)));
    stopAfterTask_ = true;
    // The server stop must not wait for a synchronous RTC send already in flight.
    // Join the transport only after the independent rental owner has started.
    try {
        if (checkout_.unresolved()) {
            screen_ = Screen::Checkout;
            start(Task::CheckoutStop,[this] { checkout_.stop(); return std::monostate{}; });
            checkoutStatus_ = status_ = uiText::Server::RequestingStop;
        } else {
            screen_ = Screen::Stopping;
            retryAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(exitAfterStop_ ? 1 : 10);
            start(Task::Finish, [this] {
                const auto result = guard_.finishAttempt();
                return result;
            });
            status_ = uiText::Server::Stopping;
        }
    } catch (...) {
        closeStream();
        throw;
    }
    closeStream();
}

}
