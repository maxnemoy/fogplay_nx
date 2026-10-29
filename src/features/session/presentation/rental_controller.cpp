#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"

namespace uiText = ::fog::constants::ui;

namespace fogplay {

void ApplicationController::armExitHandling() {
    if (exitAfterStop_ || shutdownDone_) return;
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
    diagnostic("Application shutdown: cancelling work; unresolved recovery records retained.");
    if (sessionPending_) {
        stopAfterTask_ = true;
        if (paired_) paired_->requestStop();
    }
    // Do not start/retry network requests after the OS requests exit. A worker
    // still owns the guards until it is joined; retain its durable intent and
    // cancel every HTTP client before any transport or future join.
    services_->cancel();
    cancelPlayStation();
    latency_.cancel();
    if (catalogApi_) catalogApi_->cancel();
    disarmExitHandling();
    closeStream();
}

void ApplicationController::pauseSessionRecovery() {
    recoveryPaused_ = true;
    checkoutAutoPoll_ = false;
    screen_ = Screen::Recovery;
    status_ = checkoutStatus_ = uiText::Server::RecoveryPaused;
    disarmExitHandling();
    diagnostic("Session recovery paused; attempts=" + std::to_string(stopRetry_.attempts()) + "; records retained; official account check required.");
}

void ApplicationController::retrySessionRecovery() {
    if (pending_.valid() || exitAfterStop_ || shutdownDone_) return;
    stopRetry_.reset();
    recoveryPaused_ = false;
    resetStopChecks_ = true;
    if (!pairedPending_ && !checkout_.unresolved() && guard_.ownsSession()) beginFinish("manual-retry");
    else recoverSession();
}

void ApplicationController::syncPending() {
    pairedPending_ = paired_ && paired_->unresolved();
    sessionPending_ = pairedPending_ || checkout_.unresolved() || guard_.persistent() ||
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
    if (paired_) paired_->requestStop();
    checkoutStatus_ = uiText::Server::StopAfterResponse;
    status_ = checkoutStatus_;
    if (!pending_.valid()) beginFinish();
}

void ApplicationController::handlePairedCheckout() {
    syncPending();
    const auto& result = paired_->result();
    // All result messages are fixed coordinator text; no provider body, endpoint,
    // credentials or caught exception payload is included in this joined snapshot.
    diagnostic("Paired checkout: provider=FH state=" + std::to_string(static_cast<int>(result.state)) +
        " rentalPresent=" + std::to_string(!result.rental.empty()) +
        " stopRequested=" + std::to_string(result.stopRequested) +
        " unresolved=" + std::to_string(pairedPending_) +
        " preparationStage=" + fog::pairedPreparationStageName(result.preparationStage) +
        " preparationError=" + (result.preparationError ? fog::repositoryErrorKindName(*result.preparationError) : "none") +
        " message=" + safeText(result.message));
    if (auto* pairing = pairedServices_ ? pairedServices_->pairing() : nullptr) {
        const auto details = pairing->diagnostics();
        if (!details.empty()) diagnostic("Paired client: " + safeText(details));
    }
    if (completePairedTransportReconnect()) return;
    if (completePairedVideoReconnect()) return;
    checkoutAutoPoll_ = false;
    checkoutStatus_ = "FogHub: " + result.message;
    status_ = checkoutStatus_;
    if (!pairedPending_) {
        stopRetry_.reset(); recoveryPaused_ = false;
        stopAfterTask_ = false;
        if (sessionPending_) {
            if (!auth_.authenticated()) start(Task::RestoreAuth,[this] { return restoreProviders(); });
            else recoverSession();
            return;
        }
        disarmExitHandling(); screen_ = Screen::PlayStation;
        status_ = result.state == fog::PairedCheckoutState::Terminal ?
            "FogHub: завершение аренды подтверждено." : checkoutStatus_;
        offerSessionRating(result);
        return;
    }
    if (result.stopRequested) {
        stopAfterTask_ = true;
        // failPaid() can initiate cleanup inside a provider operation. Include
        // that first attempt in the same budget before scheduling any retry.
        if (!stopRetry_.attempts()) stopRetry_.begin(std::chrono::steady_clock::now());
    }
    if (result.state == fog::PairedCheckoutState::Ready && !stopAfterTask_ && !result.stopRequested) {
        if (!pairedServices_->bindRental(result.rental)) { beginFinish("paired-stream-binding-failed"); return; }
        startStream(result.rental,false);
        return;
    }
    if (stopAfterTask_ && !result.stopRequested) { beginFinish("paired-stop-queued"); return; }
    if (stopAfterTask_ || result.stopRequested) {
        if (stopRetry_.exhausted(std::chrono::steady_clock::now())) { pauseSessionRecovery(); return; }
        screen_ = Screen::Checkout;
        checkoutAutoPoll_ = true;
        checkoutAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        return;
    }
    if (result.state == fog::PairedCheckoutState::Preparing && !stopAfterTask_) {
        screen_ = Screen::Checkout; checkoutAutoPoll_ = true;
        checkoutAt_ = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    } else {
        screen_ = Screen::Recovery;
        status_ = checkoutStatus_ + " A — проверить; Y — исходный аккаунт. При необходимости завершите аренду на FogHub.";
    }
}

void ApplicationController::handleCheckout() {
    syncPending();
    const auto& result = checkout_.result();
    checkoutAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    checkoutAutoPoll_ = fog::checkoutNeedsAutomaticCheck(result);
    if (result.stopRequested) checkoutAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(exitAfterStop_ ? 1 : 10);
    if (result.state == fog::CheckoutState::Ready && !stopAfterTask_ && !result.stopRequested) {
        startStream(result.rentalId,!selectedPlayStation_,selectedComputerId_); return;
    }
    if (!sessionPending_) {
        stopRetry_.reset(); recoveryPaused_ = false;
        disarmExitHandling(); stopAfterTask_ = false;
        screen_ = selectedPlayStation_ ? Screen::PlayStation : Screen::Games;
        status_ = result.state == fog::CheckoutState::Rejected ? uiText::Server::OrderRejected : uiText::Server::RentalFinished;
        return;
    }
    if ((result.stopRequested || stopAfterTask_) && stopRetry_.exhausted(std::chrono::steady_clock::now())) {
        pauseSessionRecovery(); return;
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
    if (manual && (stopAfterTask_ || recoveryPaused_ || checkout_.result().stopRequested)) {
        retrySessionRecovery(); return;
    }
    if (stopAfterTask_) { beginFinish("stop-poll"); return; }
    if (pairedPending_) {
        start(Task::PairedPoll,[this] { paired_->poll(); return std::monostate{}; });
        return;
    }
    start(Task::CheckoutPoll, [this, manual] { checkout_.poll(manual); return std::monostate{}; });
}

void ApplicationController::createPairedSession(const fog::Computer& pc) {
    if (!paired_ || !pairedServices_ || !pc.listed || !pc.launchable || !pc.hourlyPrice ||
        pc.availability != fog::Availability::Online) { status_ = uiText::Server::TariffUnconfirmedRefreshHint; return; }
    if (!pairedServices_->auth().authenticated()) {
        loginProvider_ = fog::ProviderId::FogHub; loginReturn_ = Screen::PlayStationDetails; screen_ = Screen::Consent; return;
    }
    stopRetry_.reset(); recoveryPaused_ = false;
    checkStorage(); armExitHandling(); sessionPending_ = true; stopAfterTask_ = false;
    ratingFocusLost_ = false;
    selectedPlayStation_ = false;
    screen_ = Screen::Checkout; checkoutStatus_ = "FogHub: проверяем доступность и тариф…";
    fog::StartRentalRequest request; request.computerId = pc.id;
    start(Task::PairedCreate,[this,request,offered=pc]() -> TaskResult {
        auto fresh = pairedServices_->catalog().playStationComputers();
        const auto found = std::find_if(fresh.begin(),fresh.end(),[&request](const fog::Computer& computer) {
            return computer.id == request.computerId;
        });
        if (found == fresh.end() || !found->listed || !found->launchable || found->availability != fog::Availability::Online ||
            !fog::sameOffer(*found,offered) || (!found->hourlyPrice || !offered.hourlyPrice || found->hourlyPrice->minorUnits != offered.hourlyPrice->minorUnits || found->hourlyPrice->currency != offered.hourlyPrice->currency))
            return ChangedOffer{std::move(fresh)};
        return VerifiedOffer{request};
    });
    return;
}

void ApplicationController::createSession() {
    const bool playStation = screen_ == Screen::PlayStationDetails;
    if (screen_ != Screen::Details && !playStation) return;
    const auto& computers = playStation ? playStationComputers_ : computers_;
    if (sessionPending_) { screen_ = Screen::Recovery; status_ = uiText::Server::RecoveryPaused; return; }
    if (selection_ >= computers.size()) return;
    const auto& pc = computers[selection_];
    if (pc.id.provider == fog::ProviderId::FogHub) { createPairedSession(pc); return; }
    if (playStation && (playStationLoadedAt_ == std::chrono::steady_clock::time_point{} ||
        std::chrono::steady_clock::now()-playStationLoadedAt_ > std::chrono::seconds(60))) {
        loadPlayStation();
        status_ = uiText::Server::RefreshingTariff;
        return;
    }
    fog::StartRentalRequest order;
    order.computerId = pc.id;
    order.offerId = pc.offerId;
    if (playStation && (!pc.listed || !pc.launchable ||
        !fog::isPlayStationComputerId(order.computerId) || order.offerId.empty())) {
        status_ = uiText::PlayStation::TariffUnconfirmedHint;
        return;
    }
    loginProvider_ = fog::ProviderId::Mts;
    if (!auth_.authenticated()) {
        loginReturn_ = playStation ? Screen::PlayStationDetails : Screen::Details;
        screen_ = Screen::Consent; return;
    }
    // Verify the same SD replacement path before any billable request.
    checkStorage();
    selectedPlayStation_ = playStation;
    selectedComputerId_ = pc.id;
    order.gameId = playStation ? fog::EntityId{} : selectedGameId_;
    order.ownAccount = playStation ? false : ownSteam_;
    recentLaunch_.prepare(playStation ? std::nullopt : selectedRecentGame_);
    // Lock focus/exit before the worker can send a billable create request.
    stopRetry_.reset(); recoveryPaused_ = false;
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
    if (sessionPending_) { recoverSession(); return; }
    loginProvider_ = fog::ProviderId::Mts;
    if (!auth_.authenticated()) { screen_ = Screen::Consent; return; }
    screen_ = Screen::Sessions; selection_ = 0; sessions_.clear();
    start(Task::Sessions, [this] { return rentals_.activeSessions(); });
}

void ApplicationController::recoverSession() {
    if (pending_.valid()) return;
    if (recoveryPaused_) { screen_ = Screen::Recovery; status_ = uiText::Server::RecoveryPaused; return; }
    if (pairedPending_) {
        loginProvider_ = fog::ProviderId::FogHub; screen_ = Screen::Recovery;
        if (!pairedServices_->auth().authenticated()) { status_ = "FogHub: нужен исходный аккаунт аренды. Y — профиль."; return; }
        armExitHandling(); stopAfterTask_ = true;
        beginFinish("paired-recovery");
        return;
    }
    loginProvider_ = fog::ProviderId::Mts;
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
    startStream(rental,false,sessions_[selection_].computerId.value_or(fog::EntityId{}));
}

void ApplicationController::beginFinish(const char* reason) {
    if (pending_.valid() || !sessionPending_ || exitAfterStop_ || shutdownDone_) return;
    if (recoveryPaused_ || !stopRetry_.begin(std::chrono::steady_clock::now())) {
        pauseSessionRecovery(); return;
    }
    diagnostic("Rental stop begin; reason=" + std::string(reason) + " screen=" + std::to_string(static_cast<int>(screen_)));
    stopAfterTask_ = true;
    // The server stop must not wait for a synchronous RTC send already in flight.
    // Join the transport only after the independent rental owner has started.
    try {
        if (pairedPending_) {
            paired_->requestStop(); screen_ = Screen::Recovery;
            start(Task::PairedStop,[this] { paired_->bindAuthenticatedAccount(); paired_->stop(); return std::monostate{}; });
            checkoutAutoPoll_ = false;
            status_ = "FogHub: отправляем завершение аренды…";
        } else if (checkout_.unresolved()) {
            screen_ = Screen::Checkout;
            const bool manualRetry = resetStopChecks_;
            resetStopChecks_ = false;
            start(Task::CheckoutStop,[this,manualRetry] { checkout_.stop(manualRetry); return std::monostate{}; });
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
